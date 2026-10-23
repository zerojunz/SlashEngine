// A stand-in for Blender, for the .blend tests and the blend-models example. --version prints a
// version as Blender does: FAKE_BLENDER_VERSION's, else 4.2.1 LTS. Run as SlashEngine converts a
// .blend file (--background --python-exit-code 1 --python-expr <script>), it reads the file and
// the glTF to write from the script, and "exports" a stand-in .blend file: one whose first line is
// "BLENDER-fake" and whose rest is a glTF document, written as a .glb file's JSON chunk. Each run
// is appended to the file FAKE_BLENDER_LOG names. FAKE_BLENDER_FAIL=crash makes it fail as Blender
// does when the script raises, =nothing exit without writing and =hang wait a minute first.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

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

// The Python string literal after key in the script, unescaped.
std::string python_value(const std::string& script, const std::string& key) {
    std::size_t at = script.find("'" + key + "': '");
    if (at == std::string::npos) {
        return {};
    }
    at += key.size() + 5;
    std::string out;
    for (; at < script.size() && script[at] != '\''; ++at) {
        if (script[at] == '\\' && at + 1 < script.size()) {
            ++at;
            out += script[at] == 'n' ? '\n' : script[at] == 'r' ? '\r' : script[at];
        } else {
            out += script[at];
        }
    }
    return out;
}

std::string read(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return in ? out.str() : std::string();
}

void word(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFU));
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string script;
    bool background = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--version") {
            const std::string version = environment("FAKE_BLENDER_VERSION");
            std::printf("Blender %s\n\tbuild date: 2024-08-19\n",
                        version.empty() ? "4.2.1 LTS" : version.c_str());
            return 0;
        }
        if (arg == "--background") {
            background = true;
        } else if (arg == "--python-expr" && i + 1 < argc) {
            script = argv[++i];
        }
    }
    const std::string source = python_value(script, "path");
    const std::string destination = python_value(script, "filepath");
    if (!background || source.empty() || destination.empty()) {
        std::fputs("fake blender: expected --background and an export script\n", stderr);
        return 2;
    }
    if (const std::string log = environment("FAKE_BLENDER_LOG"); !log.empty()) {
        // Binary, so lines end in '\n' on Windows too.
        std::ofstream(log, std::ios::app | std::ios::binary)
            << "export " << source << " " << destination << '\n';
    }
    std::printf("Blender 4.2.1 LTS (hash a51f293548ad built 2024-08-19)\nRead blend: \"%s\"\n",
                source.c_str());
    const std::string fail = environment("FAKE_BLENDER_FAIL");
    if (fail == "hang") {
        std::this_thread::sleep_for(std::chrono::minutes(1));
    }
    const std::string blend = read(source);
    const std::string header = "BLENDER-fake\n";
    if (fail == "crash" || blend.compare(0, header.size(), header) != 0) {
        std::puts("Traceback (most recent call last):\n"
                  "  File \"<string>\", line 7, in <module>\n"
                  "RuntimeError: Error: Cannot read file, not a blend file");
        return 1;
    }
    std::puts("INFO Draco mesh compression is available, use library at bpy/lib/extern_draco.so");
    if (fail == "nothing") {
        return 0;
    }
    std::string text = blend.substr(header.size());
    text.append((4 - (text.size() % 4)) % 4, ' ');
    std::string glb = "glTF";
    word(glb, 2);
    word(glb, static_cast<std::uint32_t>(12 + 8 + text.size()));
    word(glb, static_cast<std::uint32_t>(text.size()));
    glb += "JSON";
    glb += text;
    std::ofstream(destination, std::ios::binary) << glb;
    std::printf("Finished glTF 2.0 export in 0.01 s\n");
    return 0;
}
