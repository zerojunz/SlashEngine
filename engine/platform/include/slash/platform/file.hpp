#pragma once

#include "slash/core/result.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Whole-file reads and writes through SDL, which also reaches bundled assets on platforms such as
// Android where files are not plain paths. Neither call needs a Platform to exist.
namespace slash::platform {

[[nodiscard]] Result<std::string> read_file(std::string_view path);

// Reads path, relative to folder. If there is no such file, the error is NotFound "no such file"
// with the closest file of the same type in folder (outside build/) as a "did you mean" hint, so
// a misspelled path is easy to fix. A path with a reserved_device_name is refused unread.
[[nodiscard]] Result<std::string> read_file_in(std::string_view folder, std::string_view path);

// The first part of a relative path, such as "con" in "prefabs/con.prefab.json", that Windows
// keeps for a device: CON, PRN, AUX, NUL, COM1 to COM9 or LPT1 to LPT9, in any case and with any
// extension. Opening such a file on Windows reaches the device instead and can wait forever for
// console input, so projects refuse these names on every platform. "" if there is none.
[[nodiscard]] std::string_view reserved_device_name(std::string_view path);

// The error for a path with a reserved device name.
[[nodiscard]] Error reserved_device_error(std::string_view path);

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

// Every file under folder and its subfolders, as paths relative to folder separated by '/',
// sorted. Files and folders whose names start with '.' are skipped, and so is each subfolder for
// which descend, given its relative path such as "build/", returns false.
[[nodiscard]] Result<std::vector<std::string>>
list_files(std::string_view folder,
           const std::function<bool(std::string_view folder)>& descend = nullptr);

// The folder holding the running program's bundled files, ending in a separator: the app
// bundle's Resources folder on macOS and iOS, the executable's folder on Windows and Linux, and
// "/" on the web. Empty if the platform can't tell.
[[nodiscard]] std::string base_path();

} // namespace slash::platform
