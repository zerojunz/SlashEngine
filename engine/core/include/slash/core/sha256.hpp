#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// SHA-256, for checking downloads against their checksums and for storing files by the hash of
// their bytes, as the asset library does.
namespace slash {

class Sha256 {
public:
    void update(std::string_view data);
    // The hash of everything given to update, as 64 lowercase hex digits. Call it once.
    [[nodiscard]] std::string hex();

private:
    void compress();

    std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<unsigned char, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0;
};

// The SHA-256 of data, as 64 lowercase hex digits.
[[nodiscard]] std::string sha256_hex(std::string_view data);

} // namespace slash
