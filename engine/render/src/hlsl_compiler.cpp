#include "hlsl_compiler.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <SDL3/SDL_loadso.h>

#include <d3dcompiler.h>
#endif

#include <string>

namespace slash::render::detail {

#ifdef _WIN32

namespace {

// Loaded at run time, so building needs no import library and a missing DLL is an error, not a
// failure to start.
class Compiler {
public:
    Compiler() : library_(SDL_LoadObject("d3dcompiler_47.dll")) {
        if (library_ != nullptr) {
            compile_ = reinterpret_cast<pD3DCompile>(SDL_LoadFunction(library_, "D3DCompile"));
        }
    }
    Compiler(const Compiler&) = delete;
    Compiler& operator=(const Compiler&) = delete;
    Compiler(Compiler&&) = delete;
    Compiler& operator=(Compiler&&) = delete;
    ~Compiler() {
        if (library_ != nullptr) {
            SDL_UnloadObject(library_);
        }
    }

    [[nodiscard]] pD3DCompile get() const { return compile_; }

private:
    SDL_SharedObject* library_;
    pD3DCompile compile_ = nullptr;
};

std::string blob_text(ID3DBlob* blob) {
    if (blob == nullptr) {
        return "no details";
    }
    return {static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize()};
}

} // namespace

Result<std::vector<std::uint8_t>> compile_hlsl(std::string_view source, const char* entry,
                                               const char* target) {
    const Compiler compiler;
    if (compiler.get() == nullptr) {
        return make_error(ErrorCode::PlatformFailure,
                          "d3dcompiler_47.dll, which compiles shaders for Direct3D 12, is missing");
    }
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT result =
        compiler.get()(source.data(), source.size(), nullptr, nullptr, nullptr, entry, target,
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    std::string details = blob_text(errors);
    if (errors != nullptr) {
        errors->Release();
    }
    if (FAILED(result) || code == nullptr) {
        if (code != nullptr) {
            code->Release();
        }
        return make_error(ErrorCode::PlatformFailure,
                          std::string("compiling the ") + entry + " shader failed: " + details);
    }
    const auto* bytes = static_cast<const std::uint8_t*>(code->GetBufferPointer());
    std::vector<std::uint8_t> bytecode(bytes, bytes + code->GetBufferSize());
    code->Release();
    return bytecode;
}

#else

Result<std::vector<std::uint8_t>> compile_hlsl(std::string_view /*source*/, const char* /*entry*/,
                                               const char* /*target*/) {
    return make_error(ErrorCode::PlatformFailure, "HLSL compiles only on Windows");
}

#endif

} // namespace slash::render::detail
