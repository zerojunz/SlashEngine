#include "slash/core/sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace slash {
namespace {

constexpr std::size_t kLengthOffset = 56;
constexpr std::array<std::uint32_t, 64> kRound{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

} // namespace

void Sha256::update(std::string_view data) {
    length_ += data.size();
    while (!data.empty()) {
        const std::size_t take = std::min(data.size(), buffer_.size() - buffered_);
        std::copy_n(data.begin(), take, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
        buffered_ += take;
        data.remove_prefix(take);
        if (buffered_ == buffer_.size()) {
            compress();
            buffered_ = 0;
        }
    }
}

std::string Sha256::hex() {
    const std::uint64_t bits = length_ * 8;
    update("\x80");
    while (buffered_ != kLengthOffset) {
        update(std::string_view("\0", 1));
    }
    std::string length(8, '\0');
    for (std::size_t i = 0; i < length.size(); ++i) {
        length[i] = static_cast<char>(static_cast<unsigned char>(bits >> (56 - 8 * i)));
    }
    update(length);
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string text;
    for (const std::uint32_t word : state_) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            text += kDigits[(word >> shift) & 0xFU];
        }
    }
    return text;
}

void Sha256::compress() {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w.at(i) = (std::uint32_t{buffer_.at(4 * i)} << 24) |
                  (std::uint32_t{buffer_.at(4 * i + 1)} << 16) |
                  (std::uint32_t{buffer_.at(4 * i + 2)} << 8) |
                  std::uint32_t{buffer_.at(4 * i + 3)};
    }
    for (std::size_t i = 16; i < w.size(); ++i) {
        const std::uint32_t s0 =
            std::rotr(w.at(i - 15), 7) ^ std::rotr(w.at(i - 15), 18) ^ (w.at(i - 15) >> 3);
        const std::uint32_t s1 =
            std::rotr(w.at(i - 2), 17) ^ std::rotr(w.at(i - 2), 19) ^ (w.at(i - 2) >> 10);
        w.at(i) = w.at(i - 16) + s0 + w.at(i - 7) + s1;
    }
    auto [a, b, c, d, e, f, g, h] = state_;
    for (std::size_t i = 0; i < w.size(); ++i) {
        const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const std::uint32_t choice = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + choice + kRound.at(i) + w.at(i);
        const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + s0 + majority;
    }
    const std::array<std::uint32_t, 8> added{a, b, c, d, e, f, g, h};
    for (std::size_t i = 0; i < state_.size(); ++i) {
        state_.at(i) += added.at(i);
    }
}

std::string sha256_hex(std::string_view data) {
    Sha256 hash;
    hash.update(data);
    return hash.hex();
}

} // namespace slash
