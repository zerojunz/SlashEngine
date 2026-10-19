#include "compression.hpp"

#include <algorithm>
#include <array>
#include <brotli/decode.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace slash::importer {
namespace {

// Decompressed files over this are refused rather than filling memory.
constexpr std::size_t kMaxOutput = std::size_t{1} << 31;

Error corrupt(std::string_view what) {
    return Error{.code = ErrorCode::InvalidArgument, .message = std::string(what)};
}

// Deflate's bits, least significant first, buffered 64 at a time.
class Bits {
public:
    explicit Bits(std::string_view in) : in_(in) {}

    std::uint32_t peek(int count) {
        fill();
        return static_cast<std::uint32_t>(buffer_ & ((std::uint64_t{1} << count) - 1));
    }

    void drop(int count) {
        if (count > held_) {
            overrun_ = true;
            buffer_ = 0;
            held_ = 0;
            return;
        }
        buffer_ >>= count;
        held_ -= count;
    }

    std::uint32_t take(int count) {
        const std::uint32_t value = peek(count);
        drop(count);
        return value;
    }

    // Skips to the next whole byte, as a stored block starts on one.
    void align() { drop(held_ % 8); }

    // Copies bytes after align(): those still buffered, then the rest straight from the input.
    bool copy(std::size_t count, std::string& out) {
        while (count > 0 && held_ >= 8) {
            out.push_back(static_cast<char>(buffer_ & 0xFFU));
            buffer_ >>= 8;
            held_ -= 8;
            --count;
        }
        if (count > in_.size() - next_) {
            return false;
        }
        out.append(in_.substr(next_, count));
        next_ += count;
        return true;
    }

    [[nodiscard]] bool overrun() const noexcept { return overrun_; }
    // The input bytes used so far, counting a partly used last byte.
    [[nodiscard]] std::size_t consumed() const noexcept {
        return next_ - (static_cast<std::size_t>(held_) / 8);
    }

private:
    void fill() {
        while (held_ <= 56 && next_ < in_.size()) {
            buffer_ |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(in_[next_++])) << held_;
            held_ += 8;
        }
    }

    std::string_view in_;
    std::size_t next_ = 0;
    std::uint64_t buffer_ = 0;
    int held_ = 0;
    bool overrun_ = false;
};

constexpr int kMaxBits = 15;
constexpr int kFastBits = 9;

// A canonical Huffman code: how many codes each length has, the symbols in code order, and a
// table that decodes codes of up to kFastBits bits in one look.
struct Huffman {
    std::array<std::uint16_t, kMaxBits + 1> count{};
    std::array<std::uint16_t, 288> symbol{};
    std::array<std::uint16_t, std::size_t{1} << kFastBits> fast{}; // symbol << 4 | length
};

std::uint32_t reverse(std::uint32_t code, int length) {
    std::uint32_t out = 0;
    for (int i = 0; i < length; ++i) {
        out = (out << 1) | ((code >> i) & 1U);
    }
    return out;
}

// Builds a code from each symbol's length. Fails if the lengths make too many codes; an
// incomplete code is allowed, as deflate's lone distance code is, and a code it lacks fails
// when decoded.
bool build(Huffman& code, const std::uint8_t* lengths, int symbols) {
    code.count.fill(0);
    for (int i = 0; i < symbols; ++i) {
        ++code.count.at(lengths[i]);
    }
    code.count[0] = 0;
    int left = 1;
    for (std::size_t length = 1; length <= std::size_t{kMaxBits}; ++length) {
        left = (left << 1) - code.count.at(length);
        if (left < 0) {
            return false;
        }
    }
    std::array<std::uint16_t, kMaxBits + 2> offsets{};
    for (std::size_t length = 1; length <= std::size_t{kMaxBits}; ++length) {
        offsets.at(length + 1) = offsets.at(length) + code.count.at(length);
    }
    for (int i = 0; i < symbols; ++i) {
        if (lengths[i] != 0) {
            code.symbol.at(offsets.at(lengths[i])++) = static_cast<std::uint16_t>(i);
        }
    }
    code.fast.fill(0);
    std::uint32_t next = 0;
    std::size_t index = 0;
    for (std::size_t length = 1; length <= std::size_t{kFastBits}; ++length) {
        for (int k = 0; k < code.count.at(length); ++k, ++next, ++index) {
            const auto entry = static_cast<std::uint16_t>(
                (static_cast<std::size_t>(code.symbol.at(index)) << 4U) | length);
            for (std::uint32_t at = reverse(next, static_cast<int>(length)); at < code.fast.size();
                 at += 1U << length) {
                code.fast.at(at) = entry;
            }
        }
        next <<= 1;
    }
    return true;
}

// The next symbol, or -1 for a code the table lacks.
int decode(Bits& in, const Huffman& code) {
    if (const std::uint16_t entry = code.fast.at(in.peek(kFastBits)); entry != 0) {
        in.drop(entry & 15);
        return entry >> 4;
    }
    // Longer codes, a bit at a time, as zlib's puff decodes them.
    int bits = 0;
    int first = 0;
    int index = 0;
    for (std::size_t length = 1; length <= std::size_t{kMaxBits}; ++length) {
        bits |= static_cast<int>(in.take(1));
        const int count = code.count.at(length);
        if (bits - count < first) {
            const int at = index + (bits - first);
            return code.symbol.at(static_cast<std::size_t>(at));
        }
        index += count;
        first = (first + count) << 1;
        bits <<= 1;
    }
    return -1;
}

constexpr std::array<std::uint16_t, 29> kLengthBase{3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                                    15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                                    67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> kLengthExtra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistanceBase{
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> kDistanceExtra{0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                                      4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                                      9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// One block's literals and matches, up to its end code.
Result<void> codes(Bits& in, const Huffman& lengths, const Huffman& distances, std::string& out) {
    for (;;) {
        const int symbol = decode(in, lengths);
        if (symbol < 0 || in.overrun()) {
            return corrupt("bad deflate data: an unknown code");
        }
        if (symbol < 256) {
            out.push_back(static_cast<char>(symbol));
            continue;
        }
        if (symbol == 256) {
            return {};
        }
        const auto index = static_cast<std::size_t>(symbol - 257);
        if (index >= kLengthBase.size()) {
            return corrupt("bad deflate data: an unknown length");
        }
        const std::size_t length = kLengthBase.at(index) + in.take(kLengthExtra.at(index));
        const int distance_symbol = decode(in, distances);
        if (distance_symbol < 0 || static_cast<std::size_t>(distance_symbol) >= 30) {
            return corrupt("bad deflate data: an unknown distance");
        }
        const auto d = static_cast<std::size_t>(distance_symbol);
        const std::size_t distance = kDistanceBase.at(d) + in.take(kDistanceExtra.at(d));
        if (distance > out.size() || in.overrun()) {
            return corrupt("bad deflate data: a match before the start");
        }
        if (out.size() + length > kMaxOutput) {
            return corrupt("the decompressed file is over 2 GiB");
        }
        const std::size_t start = out.size();
        out.resize(start + length);
        char* data = out.data();
        for (std::size_t i = 0; i < length; ++i) {
            data[start + i] = data[start - distance + i]; // overlapping copies repeat
        }
    }
}

Result<void> fixed_block(Bits& in, std::string& out) {
    static const auto tables = [] {
        std::array<std::uint8_t, 288> lengths{};
        // RFC 1951's fixed code lengths: 8 bits, then 9 from 144, 7 from 256 and 8 from 280.
        lengths.fill(8);
        std::fill(lengths.begin() + 144, lengths.begin() + 256, std::uint8_t{9});
        std::fill(lengths.begin() + 256, lengths.begin() + 280, std::uint8_t{7});
        auto codes = std::make_unique<std::array<Huffman, 2>>();
        build(codes->at(0), lengths.data(), 288);
        lengths.fill(5);
        build(codes->at(1), lengths.data(), 30);
        return codes;
    }();
    return codes(in, tables->at(0), tables->at(1), out);
}

Result<void> dynamic_block(Bits& in, std::string& out) {
    const auto literals = static_cast<int>(in.take(5) + 257);
    const auto distances = static_cast<int>(in.take(5) + 1);
    const auto code_lengths = static_cast<int>(in.take(4) + 4);
    if (literals > 286 || distances > 30) {
        return corrupt("bad deflate data: too many codes");
    }
    static constexpr std::array<std::uint8_t, 19> kOrder{16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                         11, 4,  12, 3, 13, 2, 14, 1, 15};
    std::array<std::uint8_t, 320> lengths{};
    for (int i = 0; i < code_lengths; ++i) {
        lengths.at(kOrder.at(static_cast<std::size_t>(i))) = static_cast<std::uint8_t>(in.take(3));
    }
    Huffman length_code;
    if (!build(length_code, lengths.data(), 19)) {
        return corrupt("bad deflate data: a bad code length code");
    }
    lengths.fill(0);
    for (int i = 0; i < literals + distances;) {
        const int symbol = decode(in, length_code);
        if (symbol < 0 || in.overrun()) {
            return corrupt("bad deflate data: a bad code length");
        }
        if (symbol < 16) {
            lengths.at(static_cast<std::size_t>(i++)) = static_cast<std::uint8_t>(symbol);
            continue;
        }
        std::uint8_t value = 0;
        std::uint32_t repeat = 0;
        if (symbol == 16) {
            if (i == 0) {
                return corrupt("bad deflate data: a repeat with nothing before it");
            }
            value = lengths.at(static_cast<std::size_t>(i - 1));
            repeat = 3 + in.take(2);
        } else if (symbol == 17) {
            repeat = 3 + in.take(3);
        } else {
            repeat = 11 + in.take(7);
        }
        if (i + static_cast<int>(repeat) > literals + distances) {
            return corrupt("bad deflate data: too many code lengths");
        }
        for (std::uint32_t r = 0; r < repeat; ++r) {
            lengths.at(static_cast<std::size_t>(i++)) = value;
        }
    }
    if (lengths[256] == 0) {
        return corrupt("bad deflate data: no end code");
    }
    Huffman literal_code;
    Huffman distance_code;
    if (!build(literal_code, lengths.data(), literals) ||
        !build(distance_code, lengths.data() + literals, distances)) {
        return corrupt("bad deflate data: a bad code");
    }
    return codes(in, literal_code, distance_code, out);
}

std::uint32_t read_u32_le(std::string_view bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at + i])) << (8 * i);
    }
    return value;
}

} // namespace

std::uint32_t crc32(std::string_view bytes) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> out{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
            }
            out.at(n) = c;
        }
        return out;
    }();
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const char c : bytes) {
        crc = table.at((crc ^ static_cast<std::uint8_t>(c)) & 0xFFU) ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

Result<std::string> inflate(std::string_view bytes, std::size_t& consumed) {
    Bits in(bytes);
    std::string out;
    bool last = false;
    while (!last) {
        last = in.take(1) == 1;
        const std::uint32_t type = in.take(2);
        if (in.overrun()) {
            return corrupt("bad deflate data: it ends early");
        }
        Result<void> done;
        if (type == 0) {
            in.align();
            const std::uint32_t length = in.take(16);
            const std::uint32_t check = in.take(16);
            if (in.overrun() || (length ^ 0xFFFFU) != check) {
                return corrupt("bad deflate data: a stored block's length is damaged");
            }
            if (out.size() + length > kMaxOutput) {
                return corrupt("the decompressed file is over 2 GiB");
            }
            if (!in.copy(length, out)) {
                return corrupt("bad deflate data: it ends early");
            }
        } else if (type == 1) {
            done = fixed_block(in, out);
        } else if (type == 2) {
            done = dynamic_block(in, out);
        } else {
            return corrupt("bad deflate data: an unknown block type");
        }
        if (!done) {
            return done.error();
        }
    }
    consumed = in.consumed();
    return out;
}

Result<std::string> gunzip(std::string_view bytes) {
    std::string out;
    std::size_t at = 0;
    while (at < bytes.size()) {
        const std::string_view member = bytes.substr(at);
        if (member.size() < 18 || static_cast<std::uint8_t>(member[0]) != 0x1F ||
            static_cast<std::uint8_t>(member[1]) != 0x8B || member[2] != 8) {
            if (out.empty()) {
                return corrupt("not gzip data");
            }
            break; // padding after the last member
        }
        const auto flags = static_cast<std::uint8_t>(member[3]);
        std::size_t start = 10;
        if ((flags & 0x04U) != 0) { // FEXTRA
            if (start + 2 > member.size()) {
                return corrupt("bad gzip data: it ends early");
            }
            start +=
                2 + (static_cast<std::uint8_t>(member[start]) |
                     (static_cast<std::size_t>(static_cast<std::uint8_t>(member[start + 1])) << 8));
        }
        for (const std::uint8_t text :
             {std::uint8_t{0x08}, std::uint8_t{0x10}}) { // FNAME, FCOMMENT
            if ((flags & text) != 0) {
                const std::size_t end = member.find('\0', start);
                if (end == std::string_view::npos) {
                    return corrupt("bad gzip data: it ends early");
                }
                start = end + 1;
            }
        }
        if ((flags & 0x02U) != 0) { // FHCRC
            start += 2;
        }
        if (start > member.size()) {
            return corrupt("bad gzip data: it ends early");
        }
        std::size_t used = 0;
        auto data = inflate(member.substr(start), used);
        if (!data) {
            return data.error();
        }
        const std::size_t trailer = start + used;
        if (trailer + 8 > member.size()) {
            return corrupt("bad gzip data: it ends early");
        }
        if (read_u32_le(member, trailer) != crc32(data.value()) ||
            read_u32_le(member, trailer + 4) != static_cast<std::uint32_t>(data.value().size())) {
            return corrupt("bad gzip data: its checksum doesn't match");
        }
        if (out.size() + data.value().size() > kMaxOutput) {
            return corrupt("the decompressed file is over 2 GiB");
        }
        out += data.value();
        at += trailer + 8;
    }
    return out;
}

Result<std::string> unbrotli(std::string_view bytes) {
    BrotliDecoderState* state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (state == nullptr) {
        return make_error(ErrorCode::PlatformFailure, "could not start the brotli decoder");
    }
    std::string out;
    std::array<std::uint8_t, 65536> chunk{};
    std::size_t available_in = bytes.size();
    const auto* next_in = reinterpret_cast<const std::uint8_t*>(bytes.data());
    BrotliDecoderResult result = BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT;
    while (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
        std::size_t available_out = chunk.size();
        std::uint8_t* next_out = chunk.data();
        result = BrotliDecoderDecompressStream(state, &available_in, &next_in, &available_out,
                                               &next_out, nullptr);
        out.append(reinterpret_cast<const char*>(chunk.data()), chunk.size() - available_out);
        if (out.size() > kMaxOutput) {
            BrotliDecoderDestroyInstance(state);
            return corrupt("the decompressed file is over 2 GiB");
        }
    }
    const BrotliDecoderErrorCode error = BrotliDecoderGetErrorCode(state);
    BrotliDecoderDestroyInstance(state);
    if (result != BROTLI_DECODER_RESULT_SUCCESS) {
        return corrupt(result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT
                           ? "bad brotli data: it ends early"
                           : std::string("bad brotli data: ") + BrotliDecoderErrorString(error));
    }
    return out;
}

} // namespace slash::importer
