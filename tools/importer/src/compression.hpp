#pragma once

#include "slash/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Decompressing the files games ship compressed: gzip (RFC 1952, with RFC 1951's deflate, in
// one or more members) and brotli (RFC 7932), as Unity compresses web builds.
namespace slash::importer {

// gzip's data, checking each member's CRC-32 and size.
[[nodiscard]] Result<std::string> gunzip(std::string_view bytes);

// Raw deflate data (no gzip or zlib wrapper). consumed is set to the bytes it took.
[[nodiscard]] Result<std::string> inflate(std::string_view bytes, std::size_t& consumed);

// Brotli's data.
[[nodiscard]] Result<std::string> unbrotli(std::string_view bytes);

// CRC-32 as gzip and zip compute it.
[[nodiscard]] std::uint32_t crc32(std::string_view bytes);

} // namespace slash::importer
