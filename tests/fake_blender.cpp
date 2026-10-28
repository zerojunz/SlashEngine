// A stand-in for Blender, for the .blend tests and the blend-models example. --version prints a
// version as Blender does: FAKE_BLENDER_VERSION's, else 4.2.1 LTS. Run as SlashEngine converts a
// .blend file (--background --python-exit-code 1 --python-expr <script>), it reads the file and
// the glTF to write from the script, and "exports" a stand-in .blend file: one whose first line is
// "BLENDER-fake" and whose rest is a glTF document, written as a .glb file's JSON chunk. Each run
// is appended to the file FAKE_BLENDER_LOG names. FAKE_BLENDER_FAIL=crash makes it fail as Blender
// does when the script raises, =nothing exit without writing and =hang wait a minute first.
//
// Run as SlashEngine runs an agent's build script (blend_build_script), it can't run the Python,
// so it stands in for any script with a box: params' "width", "depth" and "height" in meters
// (1 each by default), standing on the ground around the origin, in a node named params' "name"
// (default "Model"). A script with "raise " in it fails as Blender does when it raises.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// The number after "key": in a JSON object's text, or fallback.
double json_number(const std::string& json, const std::string& key, double fallback) {
    const std::size_t at = json.find("\"" + key + "\":");
    if (at == std::string::npos) {
        return fallback;
    }
    return std::strtod(json.c_str() + at + key.size() + 3, nullptr);
}

// The string after "key": in a JSON object's text, or fallback.
std::string json_string(const std::string& json, const std::string& key, std::string fallback) {
    std::size_t at = json.find("\"" + key + "\":");
    if (at == std::string::npos) {
        return fallback;
    }
    at = json.find('"', at + key.size() + 3);
    const std::size_t end = json.find('"', at + 1);
    return at == std::string::npos || end == std::string::npos ? fallback
                                                               : json.substr(at + 1, end - at - 1);
}

// A .glb file of a box width by height by depth, on the ground around the origin, in a node
// named name.
std::string box_glb(float width, float height, float depth, const std::string& name) {
    const float x = width / 2.0F;
    const float z = depth / 2.0F;
    const float corners[8][3] = {{-x, 0, -z},    {x, 0, -z},       {x, 0, z},
                                 {-x, 0, z},     {-x, height, -z}, {x, height, -z},
                                 {x, height, z}, {-x, height, z}};
    // Counter-clockwise from outside, as glTF's front faces are.
    const std::uint16_t indices[36] = {0, 1, 2, 0, 2, 3, 4, 7, 6, 4, 6, 5, 0, 4, 5, 0, 5, 1,
                                       1, 5, 6, 1, 6, 2, 2, 6, 7, 2, 7, 3, 3, 7, 4, 3, 4, 0};
    std::string bin;
    for (const auto& corner : corners) {
        for (const float value : corner) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            word(bin, bits);
        }
    }
    for (const std::uint16_t index : indices) {
        bin.push_back(static_cast<char>(index & 0xFFU));
        bin.push_back(static_cast<char>(index >> 8U));
    }
    char json[1024];
    std::snprintf(json, sizeof(json),
                  "{\"asset\":{\"version\":\"2.0\",\"generator\":\"fake blender\"},"
                  "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                  "\"nodes\":[{\"name\":\"%s\",\"mesh\":0}],"
                  "\"meshes\":[{\"name\":\"%s\",\"primitives\":[{\"attributes\":"
                  "{\"POSITION\":0},\"indices\":1}]}],"
                  "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":8,"
                  "\"type\":\"VEC3\",\"min\":[%g,0,%g],\"max\":[%g,%g,%g]},"
                  "{\"bufferView\":1,\"componentType\":5123,\"count\":36,"
                  "\"type\":\"SCALAR\"}],"
                  "\"bufferViews\":[{\"buffer\":0,\"byteLength\":96},"
                  "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":72}],"
                  "\"buffers\":[{\"byteLength\":168}]}",
                  name.c_str(), name.c_str(), static_cast<double>(-x), static_cast<double>(-z),
                  static_cast<double>(x), static_cast<double>(height), static_cast<double>(z));
    std::string text = json;
    text.append((4 - (text.size() % 4)) % 4, ' ');
    std::string glb = "glTF";
    word(glb, 2);
    word(glb, static_cast<std::uint32_t>(12 + 8 + text.size() + 8 + bin.size()));
    word(glb, static_cast<std::uint32_t>(text.size()));
    glb += "JSON";
    glb += text;
    word(glb, static_cast<std::uint32_t>(bin.size()));
    glb += std::string("BIN\0", 4);
    glb += bin;
    return glb;
}

// Stands in for running an agent's build script: writes a box sized by its params.
int build(const std::string& script) {
    const std::string code = python_value(script, "script");
    const std::string params = python_value(script, "params");
    const std::string destination = python_value(script, "filepath");
    if (const std::string log = environment("FAKE_BLENDER_LOG"); !log.empty()) {
        std::ofstream(log, std::ios::app | std::ios::binary) << "build " << destination << '\n';
    }
    std::puts("Blender 4.2.1 LTS (hash a51f293548ad built 2024-08-19)");
    if (code.find("raise ") != std::string::npos) {
        std::puts("Traceback (most recent call last):\n"
                  "  File \"script.py\", line 1, in <module>\n"
                  "ValueError: the script raised");
        return 1;
    }
    const auto size = [&](const char* key) {
        return static_cast<float>(json_number(params, key, 1.0));
    };
    std::ofstream(destination, std::ios::binary) << box_glb(
        size("width"), size("height"), size("depth"), json_string(params, "name", "Model"));
    std::printf("Finished glTF 2.0 export in 0.01 s\n");
    return 0;
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
    if (background && script.find("'script': '") != std::string::npos) {
        return build(script);
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
