#pragma once

#include "slash/core/result.hpp"

#include <cstdint>
#include <optional>
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

// When a file last changed, and its size. Hot reload compares stamps to notice edits.
struct FileStamp {
    std::int64_t modified = 0; // nanoseconds since 1970
    std::uint64_t size = 0;
    bool operator==(const FileStamp&) const = default;
};

// The file's stamp, or nullopt if there is no file at path.
[[nodiscard]] std::optional<FileStamp> file_stamp(std::string_view path);

// The folder holding the running program's bundled files, ending in a separator: the app
// bundle's Resources folder on macOS and iOS, the executable's folder on Windows and Linux, and
// "/" on the web. Empty if the platform can't tell.
[[nodiscard]] std::string base_path();

} // namespace slash::platform
