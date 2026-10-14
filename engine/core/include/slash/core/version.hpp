#pragma once

#include <string_view>

namespace slash {

// The engine version from the top-level CMake project, e.g. "0.1.0".
[[nodiscard]] std::string_view version() noexcept;

} // namespace slash
