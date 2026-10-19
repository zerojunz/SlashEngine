#include "../tools/importer/src/compression.hpp"

#include "slash/importer/unity_build.hpp"
#include "slash/platform/file.hpp"

#include <SDL3/SDL_stdinc.h>
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace importer = slash::importer;
namespace unity = slash::importer::unity;

namespace {

// Raw deflate data from zlib, one block of each kind: stored, fixed codes and dynamic codes.
// kDynamic is kText compressed.
constexpr std::array<std::uint8_t, 27> kStored{
    0x01, 0x16, 0x00, 0xe9, 0xff, 0x73, 0x74, 0x6f, 0x72, 0x65, 0x64, 0x2c, 0x20, 0x6e,
    0x6f, 0x74, 0x20, 0x63, 0x6f, 0x6d, 0x70, 0x72, 0x65, 0x73, 0x73, 0x65, 0x64,
};
constexpr std::array<std::uint8_t, 19> kFixed{
    0x4b, 0xcb, 0xac, 0x48, 0x4d, 0x51, 0x48, 0xce, 0x4f, 0x49,
    0x2d, 0xb6, 0x52, 0x48, 0x4c, 0x4a, 0x46, 0x46, 0x00,
};
constexpr std::array<std::uint8_t, 332> kDynamic{
    0x0b, 0xc9, 0x48, 0x55, 0x28, 0x2c, 0xcd, 0x4c, 0xce, 0x56, 0x48, 0x2a, 0xca, 0x2f, 0xcf, 0x53,
    0x48, 0xcb, 0xaf, 0x50, 0xc8, 0x2a, 0xcd, 0x2d, 0x28, 0x56, 0xc8, 0x2f, 0x4b, 0x2d, 0x52, 0x28,
    0x01, 0x4a, 0xe7, 0x24, 0x56, 0x55, 0x2a, 0xa4, 0xe4, 0xa7, 0xeb, 0x29, 0x84, 0x8c, 0x2a, 0x1e,
    0x55, 0x3c, 0xaa, 0x78, 0x54, 0xf1, 0xa8, 0xe2, 0x51, 0xc5, 0xc3, 0x4b, 0x31, 0x03, 0x23, 0x13,
    0x33, 0x0b, 0x2b, 0x1b, 0x3b, 0x07, 0x27, 0x17, 0x37, 0x0f, 0x2f, 0x1f, 0xbf, 0x80, 0xa0, 0x90,
    0xb0, 0x88, 0xa8, 0x98, 0xb8, 0x84, 0xa4, 0x94, 0xb4, 0x8c, 0xac, 0x9c, 0xbc, 0x82, 0xa2, 0x92,
    0xb2, 0x8a, 0xaa, 0x9a, 0xba, 0x86, 0xa6, 0x96, 0xb6, 0x8e, 0xae, 0x9e, 0xbe, 0x81, 0xa1, 0x91,
    0xb1, 0x89, 0xa9, 0x99, 0xb9, 0x85, 0xa5, 0x95, 0xb5, 0x8d, 0xad, 0x9d, 0xbd, 0x83, 0xa3, 0x93,
    0xb3, 0x8b, 0xab, 0x9b, 0xbb, 0x87, 0xa7, 0x97, 0xb7, 0x8f, 0xaf, 0x9f, 0x7f, 0x40, 0x60, 0x50,
    0x70, 0x48, 0x68, 0x58, 0x78, 0x44, 0x64, 0x54, 0x74, 0x4c, 0x6c, 0x5c, 0x7c, 0x42, 0x62, 0x52,
    0x72, 0x4a, 0x6a, 0x5a, 0x7a, 0x46, 0x66, 0x56, 0x76, 0x4e, 0x6e, 0x5e, 0x7e, 0x41, 0x61, 0x51,
    0x71, 0x49, 0x69, 0x59, 0x79, 0x45, 0x65, 0x55, 0x75, 0x4d, 0x6d, 0x5d, 0x7d, 0x43, 0x63, 0x53,
    0x73, 0x4b, 0x6b, 0x5b, 0x7b, 0x47, 0x67, 0x57, 0x77, 0x4f, 0x6f, 0x5f, 0xff, 0x84, 0x89, 0x93,
    0x26, 0x4f, 0x99, 0x3a, 0x6d, 0xfa, 0x8c, 0x99, 0xb3, 0x66, 0xcf, 0x99, 0x3b, 0x6f, 0xfe, 0x82,
    0x85, 0x8b, 0x16, 0x2f, 0x59, 0xba, 0x6c, 0xf9, 0x8a, 0x95, 0xab, 0x56, 0xaf, 0x59, 0xbb, 0x6e,
    0xfd, 0x86, 0x8d, 0x9b, 0x36, 0x6f, 0xd9, 0xba, 0x6d, 0xfb, 0x8e, 0x9d, 0xbb, 0x76, 0xef, 0xd9,
    0xbb, 0x6f, 0xff, 0x81, 0x83, 0x87, 0x0e, 0x1f, 0x39, 0x7a, 0xec, 0xf8, 0x89, 0x93, 0xa7, 0x4e,
    0x9f, 0x39, 0x7b, 0xee, 0xfc, 0x85, 0x8b, 0x97, 0x2e, 0x5f, 0xb9, 0x7a, 0xed, 0xfa, 0x8d, 0x9b,
    0xb7, 0x6e, 0xdf, 0xb9, 0x7b, 0xef, 0xfe, 0x83, 0x87, 0x8f, 0x1e, 0x3f, 0x79, 0xfa, 0xec, 0xf9,
    0x8b, 0x97, 0xaf, 0x5e, 0xbf, 0x79, 0xfb, 0xee, 0xfd, 0x87, 0x8f, 0x9f, 0x3e, 0x7f, 0xf9, 0xfa,
    0xed, 0xfb, 0x8f, 0x9f, 0xbf, 0x7e, 0xff, 0xf9, 0xfb, 0xef, 0x3f, 0x00,
};
constexpr std::array<std::uint8_t, 208> kWebDataGzip{
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0x0b, 0xcd, 0xcb, 0x2c, 0xa9, 0x0c,
    0x4f, 0x4d, 0x72, 0x49, 0x2c, 0x49, 0x34, 0xd4, 0x33, 0x60, 0x68, 0x61, 0x60, 0x00, 0x63, 0x39,
    0x20, 0xe6, 0x61, 0x00, 0x00, 0x95, 0xd7, 0x3d, 0x5d, 0x1e, 0x00, 0x00, 0x00, 0x1f, 0x8b, 0x08,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0x63, 0x60, 0x48, 0x49, 0x2c, 0x49, 0xd4, 0x2b, 0xcd,
    0xcb, 0x2c, 0xa9, 0x34, 0x4e, 0x59, 0xc4, 0xc0, 0xc0, 0x20, 0x00, 0xc4, 0xea, 0x40, 0xec, 0x99,
    0x63, 0xe4, 0x5c, 0x50, 0xe0, 0x02, 0x94, 0xd5, 0xf7, 0x4d, 0x2d, 0x49, 0x04, 0x29, 0xd3, 0x4f,
    0xcf, 0xc9, 0x4f, 0x4a, 0xcc, 0xd1, 0xcd, 0x85, 0xf2, 0xf5, 0x80, 0xc4, 0x26, 0xa0, 0x52, 0x07,
    0x36, 0x06, 0x06, 0x49, 0x20, 0x1d, 0x5c, 0x52, 0x94, 0x9a, 0x98, 0x9b, 0x99, 0x97, 0xee, 0x58,
    0x5c, 0x9c, 0x5a, 0x52, 0xac, 0x9f, 0x97, 0x5f, 0x92, 0x5a, 0xac, 0x57, 0x52, 0x51, 0x12, 0x0a,
    0x32, 0xde, 0x2d, 0x18, 0xa8, 0x84, 0x81, 0xc3, 0x54, 0xaf, 0x42, 0xaf, 0x82, 0xc1, 0xc8, 0xc0,
    0xc8, 0x48, 0xcf, 0x58, 0xcf, 0xc8, 0x20, 0xcd, 0x90, 0x61, 0xbd, 0xf4, 0xc6, 0x5f, 0x0c, 0x48,
    0x20, 0x23, 0xbf, 0x40, 0x81, 0x08, 0xcc, 0x35, 0xaa, 0x6e, 0x54, 0xdd, 0xa8, 0xba, 0x51, 0x75,
    0xa3, 0xea, 0x46, 0xd5, 0x21, 0xab, 0x03, 0x00, 0x8d, 0x98, 0x5c, 0xf6, 0xd4, 0x06, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, 193> kWebDataBrotli{
    0x6b, 0x8d, 0x00, 0x55, 0x6e, 0x69, 0x74, 0x79, 0x57, 0x65, 0x62, 0x20, 0x43, 0x6f, 0x6d,
    0x70, 0x72, 0x65, 0x73, 0x73, 0x65, 0x64, 0x20, 0x43, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74,
    0x20, 0x28, 0x62, 0x72, 0x6f, 0x74, 0x6c, 0x69, 0x29, 0x11, 0x6f, 0x00, 0xc0, 0x48, 0xe9,
    0x22, 0xee, 0x19, 0xb0, 0xce, 0x4b, 0x0f, 0x57, 0xa8, 0x7d, 0x02, 0xbb, 0x06, 0x26, 0x66,
    0x80, 0x7d, 0x29, 0x2d, 0x14, 0xd5, 0x86, 0xbe, 0xd9, 0x8e, 0xdf, 0x6c, 0x77, 0xa3, 0xb9,
    0xee, 0x36, 0x58, 0x49, 0x12, 0x74, 0x45, 0x61, 0x38, 0xf4, 0xc0, 0x13, 0xa1, 0xdc, 0xc6,
    0xb6, 0x4f, 0x24, 0x0c, 0xa2, 0xd3, 0x09, 0xf5, 0x25, 0x6a, 0x3f, 0x07, 0xd3, 0xd1, 0x4b,
    0x10, 0x0f, 0x5c, 0x1f, 0x2f, 0x00, 0x06, 0x38, 0xb5, 0x16, 0x77, 0x6f, 0xe2, 0x19, 0x4d,
    0xde, 0x00, 0x24, 0xd8, 0xa9, 0xbe, 0x09, 0x8b, 0xc3, 0xa1, 0xf3, 0x78, 0xed, 0xe9, 0x65,
    0x68, 0xf5, 0xe3, 0xd0, 0x9d, 0x6d, 0x2d, 0x5c, 0x41, 0xfe, 0x00, 0x52, 0x22, 0x64, 0x58,
    0xd0, 0xd1, 0x5a, 0xee, 0xe6, 0x51, 0x4d, 0xd5, 0xcb, 0x46, 0x9c, 0x97, 0xc7, 0x65, 0x8e,
    0x51, 0xe9, 0x80, 0x32, 0x95, 0xb8, 0x0f, 0xf7, 0x81, 0xf0, 0xce, 0x16, 0xb9, 0xf2, 0x39,
    0x0b, 0xf0, 0xf1, 0x0b, 0xba, 0x5d, 0xec, 0x0f, 0x16, 0xbe, 0x74, 0x8c, 0x17,
};
constexpr std::array<std::uint8_t, 155> kWebDataPlainBrotli{
    0x1b, 0xf1, 0x06, 0x00, 0x8c, 0x94, 0x2e, 0xe2, 0x9e, 0x01, 0xeb, 0xbc, 0xf4, 0x70, 0x85, 0xda,
    0x27, 0xb0, 0x6b, 0x60, 0x62, 0x06, 0xd8, 0x97, 0xd2, 0x42, 0x51, 0x6d, 0xe8, 0x9b, 0xed, 0xf8,
    0xcd, 0x76, 0x37, 0x9a, 0xeb, 0x6e, 0x83, 0x95, 0x24, 0x41, 0x57, 0x14, 0x86, 0x43, 0x0f, 0x3c,
    0x11, 0xca, 0x6d, 0x6c, 0xfb, 0x44, 0xc2, 0x20, 0x3a, 0x9d, 0x50, 0x5f, 0xa2, 0xf6, 0x73, 0x30,
    0x1d, 0xbd, 0x04, 0xf1, 0xc0, 0xf5, 0xf1, 0x02, 0x60, 0x80, 0x53, 0x6b, 0x71, 0xf7, 0x26, 0x9e,
    0xd1, 0xe4, 0x0d, 0x40, 0x82, 0x9d, 0xea, 0x9b, 0xb0, 0x38, 0x1c, 0x3a, 0x8f, 0xd7, 0x9e, 0x5e,
    0x86, 0x56, 0x3f, 0x0e, 0xdd, 0xd9, 0xd6, 0xc2, 0x15, 0xe4, 0x0f, 0x20, 0x25, 0x42, 0x86, 0x05,
    0x1d, 0xad, 0xe5, 0x6e, 0x1e, 0xd5, 0x54, 0xbd, 0x6c, 0xc4, 0x79, 0x79, 0x5c, 0xe6, 0x18, 0x95,
    0x0e, 0x28, 0x53, 0x89, 0xfb, 0x70, 0x1f, 0x08, 0xef, 0x6c, 0x91, 0x2b, 0x9f, 0xb3, 0x00, 0x1f,
    0xbf, 0xa0, 0xdb, 0xc5, 0xfe, 0x60, 0xe1, 0x4b, 0xc7, 0x78, 0x01,
};

std::string bytes(const auto& array) {
    return {reinterpret_cast<const char*>(array.data()), array.size()};
}

// 2056 bytes: a sentence 40 times, then every byte value.
std::string text() {
    std::string out;
    for (int i = 0; i < 40; ++i) {
        out += "The quick brown fox jumps over the lazy dog. ";
    }
    for (int i = 0; i < 256; ++i) {
        out += static_cast<char>(i);
    }
    return out;
}

void put_le(std::string& out, std::uint64_t value, int size) {
    for (int i = 0; i < size; ++i) {
        out += static_cast<char>((value >> (8 * i)) & 0xFFU);
    }
}

void put_be(std::string& out, std::uint64_t value, int size) {
    for (int i = size; i-- > 0;) {
        out += static_cast<char>((value >> (8 * i)) & 0xFFU);
    }
}

// A gzip member around deflate data, with a file name and the data's CRC-32 and size.
std::string gzip_member(std::string_view deflated, std::string_view data) {
    std::string out("\x1f\x8b\x08\x08\0\0\0\0\0\xff", 10);
    out += std::string("data.bin\0", 9);
    out += deflated;
    put_le(out, importer::crc32(data), 4);
    put_le(out, data.size(), 4);
    return out;
}

// data as gzip, in stored deflate blocks, as a test can write without a compressor.
std::string gzip_stored(std::string_view data) {
    std::string deflated;
    std::size_t at = 0;
    do {
        const std::size_t size = std::min<std::size_t>(data.size() - at, 0xFFFF);
        deflated += static_cast<char>(at + size == data.size() ? 1 : 0);
        put_le(deflated, size, 2);
        put_le(deflated, ~size & 0xFFFFU, 2);
        deflated += data.substr(at, size);
        at += size;
    } while (at < data.size());
    return gzip_member(deflated, data);
}

// Unity web data (UnityWebData1.0) holding files.
std::string web_data(std::initializer_list<std::pair<std::string_view, std::string_view>> files) {
    std::size_t header = 16 + 4;
    for (const auto& [name, contents] : files) {
        header += 12 + name.size();
    }
    std::string out("UnityWebData1.0\0", 16);
    put_le(out, header, 4);
    std::size_t offset = header;
    std::string body;
    for (const auto& [name, contents] : files) {
        put_le(out, offset, 4);
        put_le(out, contents.size(), 4);
        put_le(out, name.size(), 4);
        out += name;
        offset += contents.size();
        body += contents;
    }
    return out + body;
}

// The start of a UnityFS bundle as Unity writes it, with its archive flags.
std::string bundle_header(std::string_view revision, std::uint32_t flags) {
    std::string out("UnityFS\0", 8);
    put_be(out, 8, 4);
    out += std::string("5.x.x\0", 6);
    out += revision;
    out += '\0';
    put_be(out, 4096, 8);
    put_be(out, 100, 4);
    put_be(out, 200, 4);
    put_be(out, flags, 4);
    return out;
}

// The web data the kWebData vectors hold: a bundle, IL2CPP's metadata and a text file.
std::string game_web_data() {
    const std::string bundle = std::string("UnityFS\0\0\0\0\x08"
                                           "5.x.x\0"
                                           "2022.3.20f1\0",
                                           30);
    const std::string metadata = std::string("\xaf\x1b\xb1\xfa", 4) + std::string(12, '\0');
    std::string notes;
    for (int i = 0; i < 40; ++i) {
        notes += "hop hop hop hop hop hop hop hop hop hop\n";
    }
    return web_data({{"data.unity3d", bundle},
                     {"Il2CppData/Metadata/global-metadata.dat", metadata},
                     {"StreamingAssets/notes.txt", notes}});
}

void write(const fs::path& path, std::string_view contents = {}) {
    fs::create_directories(path.parent_path());
    REQUIRE(slash::platform::write_file(path.string(), contents).ok());
}

// A zip file holding empty files with these names, as an .apk or .ipa is a zip file.
std::string zip(std::initializer_list<std::string_view> names) {
    std::string local;
    std::string directory;
    for (const std::string_view name : names) {
        const std::size_t offset = local.size();
        local += std::string("PK\x03\x04", 4);
        put_le(local, 20, 2);
        put_le(local, 0, 2 + 2 + 2 + 2 + 4 + 4 + 4);
        put_le(local, name.size(), 2);
        put_le(local, 0, 2);
        local += name;
        directory += std::string("PK\x01\x02", 4);
        put_le(directory, 20, 2);
        put_le(directory, 20, 2);
        put_le(directory, 0, 2 + 2 + 2 + 2 + 4 + 4 + 4);
        put_le(directory, name.size(), 2);
        put_le(directory, 0, 2 + 2 + 2 + 2 + 4);
        put_le(directory, offset, 4);
        directory += name;
    }
    std::string out = local + directory + std::string("PK\x05\x06", 4);
    put_le(out, 0, 4);
    put_le(out, names.size(), 2);
    put_le(out, names.size(), 2);
    put_le(out, directory.size(), 4);
    put_le(out, local.size(), 4);
    put_le(out, 0, 2);
    return out;
}

} // namespace

TEST_CASE("deflate's stored, fixed and dynamic blocks inflate, and gzip checks each member") {
    std::size_t consumed = 0;
    auto stored = importer::inflate(bytes(kStored), consumed);
    REQUIRE(stored.ok());
    CHECK(stored.value() == "stored, not compressed");
    CHECK(consumed == kStored.size());
    auto fixed = importer::inflate(bytes(kFixed), consumed);
    REQUIRE(fixed.ok());
    CHECK(fixed.value() == "fixed codes: abcabcabcabcabc");
    auto dynamic = importer::inflate(bytes(kDynamic), consumed);
    REQUIRE(dynamic.ok());
    CHECK(dynamic.value() == text());
    CHECK(consumed == kDynamic.size());
    CHECK(importer::crc32(text()) == 0xFF9993E9U);
    CHECK(importer::crc32("") == 0);

    // Two members, as gzip concatenates them, read as one.
    const std::string first = gzip_member(bytes(kDynamic), text());
    const std::string second = gzip_stored("and more");
    auto both = importer::gunzip(first + second);
    REQUIRE(both.ok());
    CHECK(both.value() == text() + "and more");
    auto big = importer::gunzip(gzip_stored(std::string(70000, 'x')));
    REQUIRE(big.ok());
    CHECK(big.value() == std::string(70000, 'x'));

    std::string corrupt = first;
    corrupt[corrupt.size() - 6] ^= 1;
    CHECK_FALSE(importer::gunzip(corrupt).ok());
    CHECK_FALSE(importer::gunzip(first.substr(0, first.size() - 20)).ok());
    CHECK_FALSE(importer::gunzip("not gzip").ok());
    std::string bad_block = bytes(kStored);
    bad_block[3] ^= 1; // LEN and NLEN disagree
    CHECK_FALSE(importer::inflate(bad_block, consumed).ok());
}

TEST_CASE("Unity web data reads plain, gzipped or in Unity's brotli") {
    const std::string expected = game_web_data();
    for (const std::string& compressed :
         {expected, bytes(kWebDataGzip), bytes(kWebDataBrotli), bytes(kWebDataPlainBrotli)}) {
        auto data = unity::decompress_web_data(compressed);
        REQUIRE_MESSAGE(data.ok(), (data.ok() ? std::string() : data.error().message));
        CHECK(data.value() == expected);
    }
    auto brotli = importer::unbrotli(bytes(kWebDataBrotli));
    REQUIRE(brotli.ok());
    CHECK(brotli.value() == expected);

    auto files = unity::read_web_data(expected);
    REQUIRE(files.ok());
    REQUIRE(files.value().size() == 3);
    CHECK(files.value()[0].name == "data.unity3d");
    CHECK(unity::file_unity_version(files.value()[0].data) == "2022.3.20f1");
    CHECK(files.value()[1].name == "Il2CppData/Metadata/global-metadata.dat");
    CHECK(files.value()[1].data.size() == 16);
    CHECK(files.value()[2].data.starts_with("hop hop"));

    auto not_web = unity::decompress_web_data(gzip_stored("just some text"));
    REQUIRE_FALSE(not_web.ok());
    CHECK(not_web.error().message ==
          "not Unity web data: once decompressed it doesn't start with UnityWebData1.0");
    CHECK_FALSE(unity::decompress_web_data("random bytes").ok());
    CHECK_FALSE(unity::read_web_data(expected.substr(0, 60)).ok());
}

TEST_CASE("a Unity file's header says which Unity made it, and whether a bundle is encrypted") {
    CHECK(unity::file_unity_version(bundle_header("2021.3.5f1", 0x43)) == "2021.3.5f1");
    // A serialized file: format 17 has its version after the endianness byte, 22 after the
    // 64-bit sizes.
    std::string v17;
    put_be(v17, 1000, 4);
    put_be(v17, 5000, 4);
    put_be(v17, 17, 4);
    put_be(v17, 4096, 4);
    v17 += std::string(4, '\0');
    v17 += std::string("2019.4.40f1\0", 12);
    CHECK(unity::file_unity_version(v17) == "2019.4.40f1");
    std::string v22;
    put_be(v22, 0, 4);
    put_be(v22, 0, 4);
    put_be(v22, 22, 4);
    put_be(v22, 0, 4);
    v22 += std::string(4, '\0');
    put_be(v22, 1000, 4);
    put_be(v22, 5000, 8);
    put_be(v22, 4096, 8);
    put_be(v22, 0, 8);
    v22 += std::string("6000.0.23f1\0", 12);
    CHECK(unity::file_unity_version(v22) == "6000.0.23f1");
    CHECK(unity::file_unity_version("plain text, not a Unity file").empty());
    CHECK(unity::file_unity_version(std::string(64, '\0')).empty());

    // UnityCN marks bundles with 0x200 before Unity used that flag itself, and 0x1400 since.
    CHECK(unity::encrypted_bundle(bundle_header("2019.4.40f1", 0x243)));
    CHECK(unity::encrypted_bundle(bundle_header("2020.3.33f1", 0x243)));
    CHECK_FALSE(unity::encrypted_bundle(bundle_header("2020.3.34f1", 0x243)));
    CHECK_FALSE(unity::encrypted_bundle(bundle_header("2022.3.20f1", 0x243)));
    CHECK(unity::encrypted_bundle(bundle_header("2022.3.20f1", 0x1443)));
    CHECK_FALSE(unity::encrypted_bundle(bundle_header("2019.4.40f1", 0x43)));
    CHECK_FALSE(unity::encrypted_bundle(v17));
}

TEST_CASE("Unity builds are found by their layout on every platform") {
    const fs::path root = "unity_build_layouts";
    fs::remove_all(root);
    // Windows, Mono.
    write(root / "win/Hopper.exe");
    write(root / "win/UnityPlayer.dll");
    write(root / "win/Hopper_Data/globalgamemanagers");
    write(root / "win/Hopper_Data/Managed/Assembly-CSharp.dll");
    // Linux, IL2CPP.
    write(root / "linux/Hopper.x86_64");
    write(root / "linux/GameAssembly.so");
    write(root / "linux/Hopper_Data/data.unity3d");
    write(root / "linux/Hopper_Data/il2cpp_data/Metadata/global-metadata.dat");
    // macOS, in a folder beside other files.
    write(root / "mac/Hopper.app/Contents/Resources/Data/globalgamemanagers");
    write(root / "mac/Hopper.app/Contents/Resources/Data/Managed/Assembly-CSharp.dll");
    write(root / "mac/README.txt");
    // Web, compressed.
    write(root / "web/index.html");
    write(root / "web/Build/Hopper.loader.js");
    write(root / "web/Build/Hopper.data.br");
    write(root / "web/Build/Hopper.wasm.br");
    // An unpacked .apk and .ipa.
    write(root / "apk/assets/bin/Data/globalgamemanagers");
    write(root / "apk/assets/bin/Data/Managed/Metadata/global-metadata.dat");
    write(root / "ipa/Payload/Hopper.app/Data/globalgamemanagers");
    // An .apk and an .ipa, and an .apk that isn't Unity's.
    write(root / "Hopper.apk", zip({"AndroidManifest.xml", "assets/bin/Data/globalgamemanagers",
                                    "lib/arm64-v8a/libil2cpp.so"}));
    write(root / "Hopper.ipa", zip({"Payload/Hopper.app/Data/globalgamemanagers"}));
    write(root / "Other.apk", zip({"AndroidManifest.xml", "assets/game.pck"}));
    // A Unity project, which isn't a build.
    write(root / "project/Assets/Scenes/Main.unity");
    write(root / "project/ProjectSettings/ProjectVersion.txt", "m_EditorVersion: 2022.3.20f1\n");

    const auto check = [](const fs::path& path, std::string_view platform, std::string_view backend,
                          const fs::path& build_root) {
        CAPTURE(path);
        const auto build = unity::find_build(path);
        REQUIRE(build.has_value());
        CHECK(build->platform == platform);
        CHECK(build->backend == backend);
        CHECK(build->root == build_root);
    };
    check(root / "win", "windows", "mono", root / "win");
    check(root / "win/Hopper.exe", "windows", "mono", root / "win");
    check(root / "win/Hopper_Data", "windows", "mono", root / "win");
    check(root / "win/Hopper_Data/globalgamemanagers", "windows", "mono", root / "win");
    check(root / "linux", "linux", "il2cpp", root / "linux");
    check(root / "mac", "macos", "mono", root / "mac/Hopper.app");
    check(root / "mac/Hopper.app", "macos", "mono", root / "mac/Hopper.app");
    check(root / "web", "web", "il2cpp", root / "web");
    check(root / "web/Build", "web", "il2cpp", root / "web");
    check(root / "web/Build/Hopper.data.br", "web", "il2cpp", root / "web");
    check(root / "apk", "android", "il2cpp", root / "apk");
    check(root / "ipa", "ios", "il2cpp", root / "ipa/Payload/Hopper.app");
    check(root / "Hopper.apk", "android", "il2cpp", root / "Hopper.apk");
    check(root / "Hopper.ipa", "ios", "il2cpp", root / "Hopper.ipa");

    const auto web = unity::find_build(root / "web");
    REQUIRE(web.has_value());
    CHECK(web->web_data == root / "web/Build/Hopper.data.br");
    CHECK(web->web_files == std::vector<fs::path>{root / "web/Build/Hopper.loader.js",
                                                  root / "web/Build/Hopper.wasm.br"});
    CHECK(unity::find_build(root / "Hopper.apk")->package);
    const auto linux_build = unity::find_build(root / "linux");
    REQUIRE(linux_build.has_value());
    CHECK(linux_build->data == root / "linux/Hopper_Data");

    CHECK_FALSE(unity::find_build(root / "Other.apk").has_value());
    CHECK_FALSE(unity::find_build(root / "project").has_value());
    CHECK_FALSE(unity::find_build(root / "mac/README.txt").has_value());
    CHECK_FALSE(unity::find_build(root / "nothing-here").has_value());
    fs::remove_all(root);
}

TEST_CASE("the scripts of Unity's packages are known by namespace and class") {
    CHECK(unity::package_script_guid("UnityEngine.UI", "Image") ==
          "fe87c0e1cc204ed48ad3b37840f39efc");
    CHECK(unity::package_script_guid("TMPro", "TextMeshProUGUI") ==
          "f4688fdb7df04437aeb418b961361dc5");
    CHECK(unity::package_script_guid("UnityEngine.EventSystems", "EventSystem") ==
          "76c392e42b5098c458856cdf6ecaaaa1");
    // URP's 2D lights moved out of its Experimental namespace in URP 11.
    CHECK(unity::package_script_guid("UnityEngine.Rendering.Universal", "Light2D") ==
          "073797afb82c5a1438f328866b10b3f0");
    CHECK(unity::package_script_guid("UnityEngine.Experimental.Rendering.Universal",
                                     "ShadowCaster2D") == "7db70e0ea77f5ac47a8f4565a9406397");
    CHECK(unity::package_script_guid("", "Image").empty());
    CHECK(unity::package_script_guid("MyGame.UI", "Button").empty());
    CHECK(unity::package_script_guid("UnityEngine.UI", "Player").empty());
}

namespace {

// A folder for unpack_build: the build, AssetRipper's work folder and what it "exports".
struct Unpacking {
    fs::path root;
    fs::path work;
    fs::path project;

    explicit Unpacking(const fs::path& folder)
        : root(fs::absolute(folder)), work(root / "work"), project(root / "ripped") {
        fs::remove_all(root);
        fs::create_directories(work);
        write(project / "Assets/Scenes/Main.unity", "%YAML 1.1\n");
        write(project / "ProjectSettings/ProjectVersion.txt", "m_EditorVersion: 2022.3.20f1\n");
        REQUIRE(SDL_setenv_unsafe("FAKE_ASSETRIPPER_PROJECT", project.string().c_str(), 1) == 0);
        REQUIRE(SDL_setenv_unsafe("FAKE_ASSETRIPPER_FAIL", "", 1) == 0);
    }
    Unpacking(const Unpacking&) = delete;
    Unpacking& operator=(const Unpacking&) = delete;
    Unpacking(Unpacking&&) = delete;
    Unpacking& operator=(Unpacking&&) = delete;
    ~Unpacking() {
        SDL_unsetenv_unsafe("FAKE_ASSETRIPPER_PROJECT");
        SDL_unsetenv_unsafe("FAKE_ASSETRIPPER_FAIL");
        std::error_code error;
        fs::remove_all(root, error);
    }

    [[nodiscard]] slash::Result<unity::UnpackedBuild> unpack(const fs::path& build) const {
        const auto found = unity::find_build(build);
        REQUIRE(found.has_value());
        return unity::unpack_build(
            *found, {.assetripper = SLASH_FAKE_ASSETRIPPER, .work = work, .start_timeout = 60});
    }

    // What the stand-in was asked to do.
    [[nodiscard]] std::string calls() const {
        auto text = slash::platform::read_file((work / "assetripper.log").string());
        return text ? text.value() : std::string();
    }
};

} // namespace

TEST_CASE("unpacking a Unity build runs AssetRipper on it and reports what came back") {
    const Unpacking unpacking("unity_unpack_folder");
    const fs::path game = unpacking.root / "Hopper";
    write(game / "Hopper.x86_64");
    write(game / "Hopper_Data/globalgamemanagers");
    write(game / "Hopper_Data/Managed/Assembly-CSharp.dll");
    auto unpacked = unpacking.unpack(game);
    REQUIRE_MESSAGE(unpacked.ok(), (unpacked.ok() ? std::string() : unpacked.error().message));
    CHECK(unpacked.value().project == unpacking.work / "export" / "ExportedProject");
    CHECK(fs::exists(unpacked.value().project / "Assets/Scenes/Main.unity"));
    CHECK(unpacking.calls() == "LoadFolder " + game.string() + "\nExport " +
                                   (unpacking.work / "export").string() + "\n");
    const auto expected = slash::json::parse(R"({
        "assetripper": "2.0.0",
        "build": {"backend": "mono", "platform": "linux", "unity": "2022.3.20f1"},
        "files": 1, "package_scripts": 0, "prefabs": 0, "scenes": 1, "scripts": 0,
        "unsupported": []
    })");
    REQUIRE(expected.ok());
    CHECK(slash::json::write(unpacked.value().report) == slash::json::write(expected.value()));
}

TEST_CASE("a compressed web build is decompressed for AssetRipper, and an .apk loaded whole") {
    const Unpacking unpacking("unity_unpack_web");
    const fs::path game = unpacking.root / "site";
    write(game / "index.html");
    write(game / "Build/Hopper.data.unityweb", bytes(kWebDataBrotli));
    write(game / "Build/Hopper.wasm.gz", gzip_stored("\0asm wasm"));
    write(game / "Build/Hopper.loader.js", "loader");
    auto unpacked = unpacking.unpack(game);
    REQUIRE_MESSAGE(unpacked.ok(), (unpacked.ok() ? std::string() : unpacked.error().message));
    const fs::path staged = unpacking.work / "web";
    CHECK(unpacking.calls().starts_with("LoadFolder " + staged.string() + "\n"));
    CHECK(slash::platform::read_file((staged / "Build/Hopper.data").string()).value() ==
          game_web_data());
    CHECK(slash::platform::read_file((staged / "Build/Hopper.wasm").string()).value() ==
          "\0asm wasm");
    CHECK(slash::platform::read_file((staged / "Build/Hopper.loader.js").string()).value() ==
          "loader");
    CHECK(unpacked.value().report.find("build")->find("platform")->as_string() == "web");

    const Unpacking apk("unity_unpack_apk");
    write(apk.root / "Hopper.apk", zip({"assets/bin/Data/globalgamemanagers"}));
    REQUIRE(apk.unpack(apk.root / "Hopper.apk").ok());
    CHECK(apk.calls().starts_with("LoadFile " + (apk.root / "Hopper.apk").string() + "\n"));
}

TEST_CASE("unpacking refuses encrypted bundles and reports AssetRipper's failures") {
    const Unpacking unpacking("unity_unpack_errors");
    const fs::path game = unpacking.root / "Hopper";
    write(game / "Hopper.x86_64");
    write(game / "Hopper_Data/globalgamemanagers");
    // A bundle that is merely padded, as Unity 2022 marks with 0x200, is fine.
    write(game / "Hopper_Data/StreamingAssets/padded.bundle", bundle_header("2022.3.20f1", 0x243));
    REQUIRE(unpacking.unpack(game).ok());
    write(game / "Hopper_Data/StreamingAssets/levels/one.bundle",
          bundle_header("2019.4.40f1", 0x243));
    auto encrypted = unpacking.unpack(game);
    REQUIRE_FALSE(encrypted.ok());
    CHECK(encrypted.error().code == slash::ErrorCode::PermissionDenied);
    CHECK(encrypted.error().message ==
          "the build's asset bundles are encrypted (Hopper_Data/StreamingAssets/levels/"
          "one.bundle), which nothing here decrypts");
    fs::remove(game / "Hopper_Data/StreamingAssets/levels/one.bundle");

    REQUIRE(SDL_setenv_unsafe("FAKE_ASSETRIPPER_FAIL", "start", 1) == 0);
    auto start = unpacking.unpack(game);
    REQUIRE_FALSE(start.ok());
    CHECK(start.error().code == slash::ErrorCode::PlatformFailure);
    CHECK(start.error().message ==
          "AssetRipper stopped before it started serving:\nUnhandled exception. "
          "System.IO.FileNotFoundException: no .NET runtime");
    REQUIRE(SDL_setenv_unsafe("FAKE_ASSETRIPPER_FAIL", "export", 1) == 0);
    auto exported = unpacking.unpack(game);
    REQUIRE_FALSE(exported.ok());
    CHECK(exported.error().message.starts_with(
        "AssetRipper failed /Export/UnityProject (HTTP 500):"));
    REQUIRE(SDL_setenv_unsafe("FAKE_ASSETRIPPER_FAIL", "", 1) == 0);
    REQUIRE(SDL_setenv_unsafe("FAKE_ASSETRIPPER_PROJECT", "", 1) == 0);
    auto empty = unpacking.unpack(game);
    REQUIRE_FALSE(empty.ok());
    CHECK(empty.error().message.starts_with("AssetRipper exported no project from Hopper:"));

    auto missing = unity::unpack_build(
        *unity::find_build(game), {.assetripper = (unpacking.root / "no-such-program").string(),
                                   .work = unpacking.work,
                                   .start_timeout = 60});
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().code == slash::ErrorCode::PlatformFailure);
    CHECK(missing.error().message.starts_with("could not start AssetRipper"));
}
