// slash-ray-msl: compiles a GLSL compute shader that traces rays (engine/render/shaders/*.comp)
// into the MSL the GPU device runs on Metal, and writes it to a C++ header. glslang makes its
// SPIR-V for Vulkan 1.2 as cmake/compile_ray_shaders.cmake does, then SPIRV-Cross its MSL, with
// the ray queries' flags and cull masks (which older SPIRV-Cross dropped) and set 0 as an
// argument buffer at [[buffer(0)]], since the reflections' 64 textures and their samplers are
// more than a Metal function's argument table holds. Sampled images named for depth are Metal's
// depth2d, since the prepass's depth is a depth texture. The header is committed, so building the
// engine needs no shader tools; the shaders target rewrites it.
//
//   slash-ray-msl <shader.comp> <header> <Name>
//   slash-ray-msl --hlsl <shader.comp> <header> <Name> <passes>
//
// The header holds k<Name>Msl, the MSL; k<Name>MslIds, each binding's [[id]] in the argument
// buffer and its sampler's (combined image samplers), else -1, which the GPU device encodes the
// argument buffer by; and k<Name>MslFrom, the FNV-1a hash of the GLSL it was made from, which a
// test holds to the shader so the header can't fall behind it.
//
// With --hlsl it writes the HLSL Direct3D 12 runs instead, from the GLSL with SLASH_D3D12 defined
// (which has no buffer addresses), for cmake/compile_ray_dxil.cmake to compile with DXC: shader
// model 6.5, with its root signature in it, a descriptor table of its CBVs, SRVs, UAVs and then
// any unbounded SRV array, and one of its samplers, each in binding order. The header holds
// k<Name>Hlsl, k<Name>HlslSlots, each binding's place in the first table and its sampler's in
// the second, else -1, k<Name>HlslSamplers, how many samplers, and k<Name>HlslFrom. Its
// "// passes:" line says how many values of the specialization constant kPass to compile it with.

#include <SPIRV/GlslangToSpv.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <iterator>
#include <map>
#include <spirv_hlsl.hpp>
#include <spirv_msl.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

// MSVC takes string literals of at most 16380 bytes, so the MSL is written as raw string
// literals of this many bytes each, which the compiler joins.
constexpr std::size_t kPiece = 8000;

std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool read_file(const std::string& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// The shader's SPIR-V, as glslangValidator -V --target-env vulkan1.2 makes it, with SLASH_D3D12
// defined for Direct3D 12; empty on errors, which go to stderr.
std::vector<std::uint32_t> compile_glsl(const std::string& path, const std::string& text,
                                        bool d3d12) {
    glslang::TShader shader(EShLangCompute);
    if (d3d12) {
        shader.setPreamble("#define SLASH_D3D12 1\n");
    }
    const char* string = text.c_str();
    const auto length = static_cast<int>(text.size());
    const char* name = path.c_str();
    shader.setStringsWithLengthsAndNames(&string, &length, &name, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangCompute, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    const auto messages =
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): glslang's flags combine.
        static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 460, false, messages)) {
        std::fprintf(stderr, "%s\n", shader.getInfoLog());
        return {};
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        std::fprintf(stderr, "%s\n", program.getInfoLog());
        return {};
    }
    std::vector<std::uint32_t> spirv;
    glslang::SpvOptions options;
    options.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), spirv, &options);
    return spirv;
}

// A binding's place in the argument buffer: its [[id]], and its sampler's.
struct Ids {
    int id = -1;
    int sampler = -1;
};

// Metal samples a depth texture only as depth2d, whose reads give a float, where SPIR-V's
// sampled images give four: name's type becomes depth2d<float>, and each of its reads is made
// four again, so the rest of the code reads it as it did.
std::string as_depth(std::string msl, std::string_view name) {
    const std::string type = "texture2d<float> " + std::string(name);
    for (std::size_t at = msl.find(type); at != std::string::npos; at = msl.find(type, at)) {
        msl.replace(at, type.size(), "depth2d<float> " + std::string(name));
    }
    const std::string read = std::string(name) + ".read(";
    for (std::size_t at = msl.find(read); at != std::string::npos;) {
        // The whole expression the read is on, such as spvDescriptorSet0.depth_texture.
        std::size_t start = at;
        while (start > 0 && (std::isalnum(static_cast<unsigned char>(msl[start - 1])) != 0 ||
                             msl[start - 1] == '_' || msl[start - 1] == '.')) {
            --start;
        }
        std::size_t end = at + read.size();
        for (int depth = 1; end < msl.size() && depth > 0; ++end) {
            if (msl[end] == '(') {
                ++depth;
            } else if (msl[end] == ')') {
                --depth;
            }
        }
        msl.insert(end, ")");
        msl.insert(start, "float4(");
        at = msl.find(read, end + 8);
    }
    return msl;
}

// The MSL of spirv, with each binding's ids.
std::string to_msl(const std::vector<std::uint32_t>& spirv, std::map<std::uint32_t, Ids>& ids) {
    spirv_cross::CompilerMSL compiler(spirv);
    auto options = compiler.get_msl_options();
    // Ray queries need MSL 2.3, and Tier 2 argument buffers hold arrays of samplers.
    options.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2, 4);
    options.argument_buffers = true;
    options.argument_buffers_tier = spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;
    options.platform = spirv_cross::CompilerMSL::Options::macOS;
    compiler.set_msl_options(options);

    // Each resource in binding order takes the next ids, as many as its array has elements; a
    // combined image sampler's samplers come after its textures.
    struct Resource {
        std::uint32_t binding = 0;
        std::uint32_t count = 1;
        bool sampled = false;
    };
    std::vector<Resource> resources;
    const spirv_cross::ShaderResources all = compiler.get_shader_resources();
    const auto add = [&](const spirv_cross::SmallVector<spirv_cross::Resource>& list,
                         bool sampled) {
        for (const spirv_cross::Resource& resource : list) {
            const spirv_cross::SPIRType& type = compiler.get_type(resource.type_id);
            Resource entry;
            entry.binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
            entry.count = type.array.empty() ? 1 : type.array[0];
            entry.sampled = sampled;
            resources.push_back(entry);
        }
    };
    add(all.uniform_buffers, false);
    add(all.storage_buffers, false);
    add(all.storage_images, false);
    add(all.sampled_images, true);
    add(all.acceleration_structures, false);
    std::sort(resources.begin(), resources.end(),
              [](const Resource& a, const Resource& b) { return a.binding < b.binding; });
    std::uint32_t next = 0;
    for (const Resource& resource : resources) {
        spirv_cross::MSLResourceBinding binding;
        binding.stage = spv::ExecutionModelGLCompute;
        binding.desc_set = 0;
        binding.binding = resource.binding;
        binding.count = resource.count;
        binding.msl_buffer = next;
        binding.msl_texture = next;
        Ids& placed = ids[resource.binding];
        placed.id = static_cast<int>(next);
        next += resource.count;
        if (resource.sampled) {
            binding.msl_sampler = next;
            placed.sampler = static_cast<int>(next);
            next += resource.count;
        }
        compiler.add_msl_resource_binding(binding);
    }
    std::string msl = compiler.compile();
    for (const spirv_cross::Resource& resource : all.sampled_images) {
        if (resource.name.find("depth") != std::string::npos) {
            msl = as_depth(std::move(msl), resource.name);
        }
    }
    return msl;
}

// The HLSL of spirv, with each binding's places in its descriptor tables and the number of
// samplers.
std::string to_hlsl(const std::vector<std::uint32_t>& spirv, std::map<std::uint32_t, Ids>& slots,
                    std::uint32_t& samplers) {
    spirv_cross::CompilerHLSL compiler(spirv);
    auto options = compiler.get_hlsl_options();
    options.shader_model = 65; // ray queries
    compiler.set_hlsl_options(options);

    enum class Kind : std::uint8_t { Cbv, Srv, Uav, Unbounded };
    struct Resource {
        std::uint32_t binding = 0;
        std::uint32_t count = 1;
        Kind kind = Kind::Srv;
        bool sampled = false;
    };
    std::vector<Resource> resources;
    const spirv_cross::ShaderResources all = compiler.get_shader_resources();
    const auto add = [&](const spirv_cross::SmallVector<spirv_cross::Resource>& list, Kind kind,
                         bool sampled) {
        for (const spirv_cross::Resource& resource : list) {
            const spirv_cross::SPIRType& type = compiler.get_type(resource.type_id);
            Resource entry;
            entry.binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
            entry.count = type.array.empty() ? 1 : type.array[0];
            entry.kind = kind;
            // SPIRV-Cross makes a buffer nothing writes a ByteAddressBuffer, an SRV.
            if (&list == &all.storage_buffers &&
                compiler.get_buffer_block_flags(resource.id).get(spv::DecorationNonWritable)) {
                entry.kind = Kind::Srv;
            }
            if (entry.count == 0) {
                if (entry.kind != Kind::Srv || sampled) {
                    throw spirv_cross::CompilerError("only arrays of read-only buffers may be "
                                                     "unbounded");
                }
                entry.kind = Kind::Unbounded;
            }
            entry.sampled = sampled;
            resources.push_back(entry);
        }
    };
    add(all.uniform_buffers, Kind::Cbv, false);
    add(all.storage_buffers, Kind::Uav, false);
    add(all.storage_images, Kind::Uav, false);
    add(all.sampled_images, Kind::Srv, true);
    add(all.acceleration_structures, Kind::Srv, false);
    std::sort(resources.begin(), resources.end(),
              [](const Resource& a, const Resource& b) { return a.binding < b.binding; });

    // The first table holds the CBVs, the SRVs, the UAVs, then any unbounded SRV array, each
    // range's registers from 0 in binding order.
    std::array<std::uint32_t, 4> counts{};
    for (const Resource& resource : resources) {
        counts.at(static_cast<std::size_t>(resource.kind)) +=
            resource.kind == Kind::Unbounded ? 1 : resource.count;
    }
    if (counts[static_cast<std::size_t>(Kind::Unbounded)] > 1) {
        throw spirv_cross::CompilerError("a shader may have one unbounded array");
    }
    const std::array<std::uint32_t, 4> first{0, counts[0], counts[0] + counts[1],
                                             counts[0] + counts[1] + counts[2]};
    std::array<std::uint32_t, 4> next{};
    samplers = 0;
    for (const Resource& resource : resources) {
        const auto kind = static_cast<std::size_t>(resource.kind);
        spirv_cross::HLSLResourceBinding binding;
        binding.stage = spv::ExecutionModelGLCompute;
        binding.desc_set = 0;
        binding.binding = resource.binding;
        // An unbounded array's registers follow the bounded SRVs'.
        const std::uint32_t reg = resource.kind == Kind::Unbounded ? counts[1] : next.at(kind);
        binding.cbv.register_binding = reg;
        binding.srv.register_binding = reg;
        binding.uav.register_binding = reg;
        Ids& placed = slots[resource.binding];
        placed.id = static_cast<int>(first.at(kind) + next.at(kind));
        next.at(kind) += resource.kind == Kind::Unbounded ? 1 : resource.count;
        if (resource.sampled) {
            binding.sampler.register_binding = samplers;
            placed.sampler = static_cast<int>(samplers);
            samplers += resource.count;
        }
        compiler.add_hlsl_resource_binding(binding);
    }
    std::string hlsl = compiler.compile();

    // The root signature, in the shader so DXC puts it in the DXIL: what the host binds may change
    // between dispatches, and unused places may be empty.
    const std::string volatile_data = ", flags = DESCRIPTORS_VOLATILE | DATA_VOLATILE)";
    std::string table;
    const auto range = [&](const char* type, const char* reg, std::uint32_t at,
                           const std::string& count) {
        table += table.empty() ? "" : ", ";
        table += std::string(type) + "(" + reg + std::to_string(at) +
                 ", numDescriptors = " + count + volatile_data;
    };
    if (counts[0] > 0) {
        range("CBV", "b", 0, std::to_string(counts[0]));
    }
    if (counts[1] > 0) {
        range("SRV", "t", 0, std::to_string(counts[1]));
    }
    if (counts[2] > 0) {
        range("UAV", "u", 0, std::to_string(counts[2]));
    }
    if (counts[3] > 0) {
        range("SRV", "t", counts[1], "unbounded");
    }
    std::string signature = "DescriptorTable(" + table + ")";
    if (samplers > 0) {
        signature += ", DescriptorTable(Sampler(s0, numDescriptors = " + std::to_string(samplers) +
                     ", flags = DESCRIPTORS_VOLATILE))";
    }
    const std::string threads = "[numthreads(";
    const std::size_t at = hlsl.find(threads);
    if (at == std::string::npos) {
        throw spirv_cross::CompilerError("the HLSL has no [numthreads]");
    }
    hlsl.insert(at, "[RootSignature(\"" + signature + "\")]\n");
    return hlsl;
}

std::string pieces(std::string_view text, std::string_view delimiter) {
    std::string out;
    for (std::size_t at = 0; at < text.size(); at += kPiece) {
        if (at > 0) {
            out += ")" + std::string(delimiter) + "\"\n    R\"" + std::string(delimiter) + "(";
        }
        out += text.substr(at, kPiece);
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    const bool hlsl = argc == 6 && std::string_view(argv[1]) == "--hlsl";
    if (argc != (hlsl ? 6 : 4)) {
        std::fprintf(stderr,
                     "usage: slash-ray-msl <shader.comp> <header> <Name>\n"
                     "       slash-ray-msl --hlsl <shader.comp> <header> <Name> <passes>\n");
        return 2;
    }
    const int first = hlsl ? 2 : 1;
    const std::string source = argv[first];
    const std::string header = argv[first + 1];
    const std::string name = argv[first + 2];
    const std::string passes = hlsl ? argv[first + 3] : "";
    std::string glsl;
    if (!read_file(source, glsl)) {
        std::fprintf(stderr, "can't read %s\n", source.c_str());
        return 1;
    }
    glslang::InitializeProcess();
    const std::vector<std::uint32_t> spirv = compile_glsl(source, glsl, hlsl);
    glslang::FinalizeProcess();
    if (spirv.empty()) {
        return 1;
    }
    std::map<std::uint32_t, Ids> ids;
    std::uint32_t samplers = 0;
    std::string code;
    try {
        code = hlsl ? to_hlsl(spirv, ids, samplers) : to_msl(spirv, ids);
    } catch (const spirv_cross::CompilerError& error) {
        std::fprintf(stderr, "SPIRV-Cross failed for %s: %s\n", source.c_str(), error.what());
        return 1;
    }

    std::uint32_t bindings = 0;
    for (const auto& [binding, placed] : ids) {
        bindings = std::max(bindings, binding + 1);
    }
    std::string table;
    for (std::uint32_t binding = 0; binding < bindings; ++binding) {
        const auto found = ids.find(binding);
        const Ids placed = found != ids.end() ? found->second : Ids{};
        table += "{" + std::to_string(placed.id) + ", " + std::to_string(placed.sampler) + "}";
        table += binding + 1 < bindings ? ", " : "";
    }
    const std::string shown = source.substr(source.find("engine/"));
    const std::string kind = hlsl ? "Hlsl" : "Msl";
    const std::string delimiter = hlsl ? "hlsl" : "msl";
    std::ostringstream out;
    out << "// clang-format off\n// Generated by slash-ray-msl "
           "(tools/shader_compiler/src/ray_msl.cpp) "
           "from "
        << shown
        << ".\n// Don't edit: change the shader, then run \"cmake --build --preset dev --target "
           "shaders\".\n";
    if (hlsl) {
        out << "// profile: cs_6_5\n// passes: " << passes << "\n";
    }
    out << "#pragma once\n\n#include <cstdint>\n\nnamespace slash::render::shaders {\n\n";
    if (hlsl) {
        std::string base = source.substr(source.find_last_of('/') + 1);
        base = base.substr(0, base.find('.'));
        out << "// The HLSL Direct3D 12 runs, as the signed DXIL DXC makes of it (" << base
            << "_dxil.hpp).\n";
    } else {
        out << "// The MSL Metal runs, its set 0 an argument buffer at [[buffer(0)]].\n";
    }
    out << "inline constexpr char k" << name << kind << "[] = R\"" << delimiter << "("
        << pieces(code, delimiter) << ")" << delimiter << "\";\n\n";
    if (hlsl) {
        out << "// Each binding's place in the descriptor table of CBVs, SRVs and UAVs, then its\n"
               "// samplers' in the table of samplers, else -1.\ninline constexpr int k"
            << name << "HlslSlots[][2] = {" << table
            << "};\n\n// The samplers' table's size.\ninline constexpr std::uint32_t k" << name
            << "HlslSamplers = " << samplers << ";\n\n";
    } else {
        out << "// Each binding's [[id]] in the argument buffer, then its samplers', else "
               "-1.\ninline constexpr int k"
            << name << "MslIds[][2] = {" << table << "};\n\n";
    }
    out << "// The FNV-1a hash of the GLSL it was made from.\ninline constexpr std::uint64_t k"
        << name << kind << "From = 0x" << std::hex << fnv1a(glsl) << std::dec
        << "ULL;\n\n} // namespace slash::render::shaders\n";
    std::ofstream file(header, std::ios::binary);
    file << out.str();
    if (!file) {
        std::fprintf(stderr, "can't write %s\n", header.c_str());
        return 1;
    }
    return 0;
}
