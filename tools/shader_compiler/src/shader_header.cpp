// slash-shader-header: compiles canvas shaders the engine builds in, written as a game's are
// (fragment() and vertex() over Godot's built-ins), into a C++ header of functions that return
// each one's render::CompiledShader. The header is committed, so games draw these shaders with
// no shader tools; the `shaders` target rewrites it, and a test in tests/test_render.cpp fails
// when it falls behind its shaders.
//
//   slash-shader-header <root> <header> <namespace>
//   <name>=<shader.hlsl>[:<param>/<n>/<slot>,...]...
//
// Each shader becomes a function of that name (snake_case), its arrays named after it.
// Each shader's path is relative to root, and is the name its errors and #line directives use.
// A param is a number param of n components read from constant slot, or a texture param when n
// is 0, as render::ShaderSource::Param.

#include "slash/platform/file.hpp"
#include "slash/shader_compiler/shader_compiler.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using slash::render::CompiledShader;
using slash::render::ShaderSource;

// snake_case as PascalCase: "fsr_easu" is "FsrEasu".
std::string pascal(std::string_view name) {
    std::string out;
    bool upper = true;
    for (const char c : name) {
        if (c == '_') {
            upper = true;
        } else {
            const bool lower = c >= 'a' && c <= 'z';
            out += upper && lower ? static_cast<char>(c - 'a' + 'A') : c;
            upper = false;
        }
    }
    return out;
}

// One shader to compile: the function's name and what it is compiled from.
struct Entry {
    std::string name;
    ShaderSource source;
};

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find(separator, start);
        out.push_back(text.substr(start, end - start));
        if (end == std::string_view::npos) {
            return out;
        }
        start = end + 1;
    }
}

// "name=path:param/n/slot,..." as an entry, its code read from under root.
bool parse_entry(std::string_view root, std::string_view spec, Entry& entry, std::string& error) {
    const std::size_t equals = spec.find('=');
    if (equals == std::string_view::npos || equals == 0) {
        error = "expected name=path, got " + std::string(spec);
        return false;
    }
    entry.name = std::string(spec.substr(0, equals));
    const std::string_view rest = spec.substr(equals + 1);
    const std::size_t colon = rest.find(':');
    entry.source.path = std::string(rest.substr(0, colon));
    if (colon != std::string_view::npos) {
        for (const std::string_view param : split(rest.substr(colon + 1), ',')) {
            const std::vector<std::string_view> parts = split(param, '/');
            if (parts.size() != 3 || parts[0].empty() || parts[1].size() != 1 || parts[2].empty()) {
                error = "expected param/components/slot, got " + std::string(param);
                return false;
            }
            entry.source.params.push_back(
                {.name = std::string(parts[0]),
                 .components = static_cast<std::uint32_t>(parts[1][0] - '0'),
                 .slot = static_cast<std::uint32_t>(std::stoul(std::string(parts[2])))});
        }
    }
    auto code = slash::platform::read_file(std::string(root) + "/" + entry.source.path);
    if (!code) {
        error = code.error().message;
        return false;
    }
    entry.source.code = std::move(code).value();
    return true;
}

// Appends a C++ array of the values, eighteen to a line.
template <typename T>
void write_array(std::string& out, std::string_view type, const std::string& name,
                 std::span<const T> values) {
    out += "inline constexpr " + std::string(type) + " " + name + "[] = {";
    for (std::size_t i = 0; i < values.size(); ++i) {
        out += i % 18 == 0 ? "\n    " : " ";
        out += std::to_string(values[i]);
        out += ',';
    }
    out += "\n};\n";
}

// Appends a C++ array of raw string literals that join to the text, each split after a line
// and well under MSVC's limit of 16380 bytes a literal.
void write_pieces(std::string& out, const std::string& name, std::string_view text) {
    constexpr std::size_t kMaxPiece = 12000;
    out += "inline constexpr const char* " + name + "[] = {\n";
    while (!text.empty()) {
        std::size_t size = std::min(text.size(), kMaxPiece);
        if (size < text.size()) {
            if (const std::size_t line = text.rfind('\n', size - 1);
                line != std::string_view::npos) {
                size = line + 1;
            }
        }
        out += "R\"slash(";
        out += text.substr(0, size);
        out += ")slash\",\n";
        text.remove_prefix(size);
    }
    out += "};\n";
}

// The header: the shaders' arrays, then a function for each that fills a CompiledShader.
std::string header(const std::vector<Entry>& entries, const std::vector<CompiledShader>& shaders,
                   std::string_view name_space) {
    std::string out = "// Generated by slash-shader-header (tools/shader_compiler) from:\n";
    for (const Entry& entry : entries) {
        out += "//   " + entry.source.path + "\n";
    }
    out += "// Don't edit: change the shaders, then run \"cmake --build --preset dev --target "
           "shaders\".\n// clang-format off\n#pragma once\n\n#include "
           "\"slash/render/shader.hpp\"\n\n#include <cstdint>\n#include <iterator>\n\nnamespace " +
           std::string(name_space) + " {\n";
    for (std::size_t s = 0; s < entries.size(); ++s) {
        const std::string& name = entries[s].name;
        const CompiledShader& shader = shaders[s];
        std::string fill;
        const auto words = [&](const std::string& field, const std::vector<std::uint32_t>& value) {
            if (value.empty()) {
                return;
            }
            const std::string array = "k" + pascal(name) + pascal(field);
            out += "\n";
            write_array<std::uint32_t>(out, "std::uint32_t", array, value);
            fill += "    shader.";
            fill += field;
            fill += ".assign(std::begin(";
            fill += array;
            fill += "), std::end(";
            fill += array;
            fill += "));\n";
        };
        const auto text = [&](const std::string& field, const std::string& value) {
            if (value.empty()) {
                return;
            }
            const std::string array = "k" + pascal(name) + pascal(field);
            out += "\n";
            write_pieces(out, array, value);
            fill += "    for (const char* piece : ";
            fill += array;
            fill += ") {\n        shader.";
            fill += field;
            fill += " += piece;\n    }\n";
        };
        words("spirv_vertex", shader.spirv_vertex);
        words("spirv_fragment", shader.spirv_fragment);
        text("msl_vertex", shader.msl_vertex);
        text("msl_fragment", shader.msl_fragment);
        text("glsl_vertex", shader.glsl_vertex);
        text("glsl_fragment", shader.glsl_fragment);
        text("wgsl_vertex", shader.wgsl_vertex);
        text("wgsl_fragment", shader.wgsl_fragment);
        text("hlsl", shader.hlsl);
        words("cpu_vertex", shader.cpu_vertex);
        words("cpu_fragment", shader.cpu_fragment);
        if (shader.normal_map_used) {
            fill += "    shader.normal_map_used = true;\n";
        }
        if (shader.specular_used) {
            fill += "    shader.specular_used = true;\n";
        }
        out += "\n// ";
        out += entries[s].source.path;
        out += "\ninline slash::render::CompiledShader ";
        out += name;
        out += "() {\n    slash::render::CompiledShader shader;\n";
        out += fill;
        out += "    return shader;\n}\n";
    }
    out += "\n} // namespace " + std::string(name_space) + "\n";
    return out;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.size() < 4) {
        std::fputs("usage: slash-shader-header <root> <header> <namespace> "
                   "<name>=<shader.hlsl>[:<param>/<n>/<slot>,...]...\n",
                   stderr);
        return 2;
    }
    std::vector<Entry> entries;
    std::vector<CompiledShader> shaders;
    for (std::size_t i = 3; i < args.size(); ++i) {
        Entry entry;
        std::string error;
        if (!parse_entry(args[0], args[i], entry, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 2;
        }
        auto compiled = slash::shader_compiler::compile(entry.source);
        if (!compiled) {
            std::fprintf(stderr, "%s\n", compiled.error().message.c_str());
            return 1;
        }
        entries.push_back(std::move(entry));
        shaders.push_back(std::move(compiled).value());
    }
    const std::string text = header(entries, shaders, args[2]);
    if (auto written = slash::platform::write_file(std::string(args[1]), text); !written) {
        std::fprintf(stderr, "%s\n", written.error().message.c_str());
        return 1;
    }
    return 0;
}
