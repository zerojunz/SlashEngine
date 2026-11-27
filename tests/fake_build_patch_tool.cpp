// A stand-in for Epic's BuildPatchTool, for the publish tests. It takes "-Name=value" arguments
// as BuildPatchTool does. -mode=UploadBinary needs the organization, product and artifact ids, a
// client whose secret is in the variable -ClientSecretEnvVar names, a -BuildRoot folder with
// files, a -CloudDir and an -AppLaunch program inside the build root. -mode=LabelBinary needs a
// -BuildVersion, -Label, -Platform and -SandboxId. Each mode appends a line to the file
// FAKE_BUILDPATCHTOOL_LOG names ("upload <client> <artifact> <version> <files> <launch>" or
// "label <artifact> <version> <label> <platform> <sandbox>"). FAKE_BUILDPATCHTOOL_FAIL names a
// mode that fails as BuildPatchTool does when Epic refuses it.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace {

std::string environment(const std::string& name) {
#if defined(_MSC_VER)
    // MSVC deprecates getenv.
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name.c_str()) != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name.c_str()); // NOLINT(concurrency-mt-unsafe): one thread
    return value == nullptr ? std::string() : std::string(value);
#endif
}

int error(const std::string& message, int code) {
    std::fprintf(stderr, "LogBuildPatchTool: Error: %s\n", message.c_str());
    std::fprintf(stderr, "LogBuildPatchTool: Error: Tool exited with: %d\n", code);
    return code;
}

void record(const std::string& line) {
    if (const std::string log = environment("FAKE_BUILDPATCHTOOL_LOG"); !log.empty()) {
        std::ofstream out(log, std::ios::app | std::ios::binary);
        out << line << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const std::size_t equals = arg.find('=');
        if (arg.size() < 2 || arg[0] != '-' || equals == std::string::npos) {
            return error("Unknown argument " + arg, 2);
        }
        args[arg.substr(1, equals - 1)] = arg.substr(equals + 1);
    }
    for (const char* name : {"OrganizationId", "ProductId", "ArtifactId", "ClientId",
                             "ClientSecretEnvVar", "mode", "BuildVersion"}) {
        if (args[name].empty()) {
            return error(std::string("Missing required parameter ") + name, 2);
        }
    }
    if (environment(args["ClientSecretEnvVar"]).empty()) {
        return error("Failed to authenticate: invalid_client_credentials", 6);
    }
    const std::string& mode = args["mode"];
    if (environment("FAKE_BUILDPATCHTOOL_FAIL") == mode) {
        return error(mode + " was refused by Epic services", 6);
    }
    if (mode == "UploadBinary") {
        const std::filesystem::path root = args["BuildRoot"];
        std::error_code failed;
        if (!std::filesystem::is_directory(root, failed)) {
            return error("Specified BuildRoot directory does not exist.", 2);
        }
        if (args["CloudDir"].empty()) {
            return error("Missing required parameter CloudDir", 2);
        }
        if (!std::filesystem::is_regular_file(root / args["AppLaunch"], failed)) {
            return error("Specified AppLaunch executable does not exist in the BuildRoot.", 2);
        }
        std::size_t files = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, failed)) {
            if (entry.is_regular_file()) {
                ++files;
            }
        }
        std::printf("Beginning chunk generation of version %s of artifact %s\n",
                    args["BuildVersion"].c_str(), args["ArtifactId"].c_str());
        record("upload " + args["ClientId"] + " " + args["ArtifactId"] + " " +
               args["BuildVersion"] + " " + std::to_string(files) + " " + args["AppLaunch"]);
    } else if (mode == "LabelBinary") {
        for (const char* name : {"Label", "Platform", "SandboxId"}) {
            if (args[name].empty()) {
                return error(std::string("Missing required parameter ") + name, 2);
            }
        }
        std::printf("Labeled binary %s as %s on %s\n", args["BuildVersion"].c_str(),
                    args["Label"].c_str(), args["Platform"].c_str());
        record("label " + args["ArtifactId"] + " " + args["BuildVersion"] + " " + args["Label"] +
               " " + args["Platform"] + " " + args["SandboxId"]);
    } else {
        return error("Unknown mode " + mode, 2);
    }
    std::puts("LogBuildPatchTool: Display: Tool exited with: 0");
    return 0;
}
