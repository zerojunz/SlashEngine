#pragma once

#include <span>
#include <string>

namespace slash::render::detail {

// Shader code that comes in pieces, joined. Compute shaders' MSL and WGSL and the code of the mesh,
// SSAO and SSR shaders (their HLSL too) are in pieces, since Clang and MSVC take no string literal
// as long as they are.
inline std::string joined(std::span<const char* const> pieces) {
    std::string text;
    for (const char* piece : pieces) {
        text += piece;
    }
    return text;
}

} // namespace slash::render::detail
