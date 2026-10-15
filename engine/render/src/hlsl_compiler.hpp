#pragma once

#include "slash/core/result.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace slash::render::detail {

// Compiles HLSL to DXBC bytecode for Direct3D 12, with the d3dcompiler_47.dll that comes with
// Windows. target is a shader model profile such as "vs_5_1". Fails on other systems.
[[nodiscard]] Result<std::vector<std::uint8_t>> compile_hlsl(std::string_view source,
                                                             const char* entry, const char* target);

} // namespace slash::render::detail
