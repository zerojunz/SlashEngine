#pragma once

#include "slash/core/result.hpp"

#include <string>
#include <string_view>

// Whole-file reads and writes through SDL, which also reaches bundled assets on platforms such as
// Android where files are not plain paths. Neither call needs a Platform to exist.
namespace slash::platform {

[[nodiscard]] Result<std::string> read_file(std::string_view path);

// Creates or replaces the file.
[[nodiscard]] Result<void> write_file(std::string_view path, std::string_view contents);

// Creates a directory and any missing parents. Succeeds if it already exists.
[[nodiscard]] Result<void> create_directories(std::string_view path);

// True if a file or directory exists at path.
[[nodiscard]] bool exists(std::string_view path);

} // namespace slash::platform
