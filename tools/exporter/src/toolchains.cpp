#include "slash/exporter/toolchains.hpp"

#include "steps.hpp"

#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace slash::exporter {
namespace {

namespace fs = std::filesystem;
using detail::environment;

// Written last into an installed toolchain's folder, so a half-unpacked one never counts.
constexpr std::string_view kMarker = ".slash-toolchain.json";

// llvm-mingw: Clang, LLD and mingw-w64 for Windows targets. Its Clang and LLD also build the
// Linux exports, against the sysroots below, since Apple's toolchain can't link ELF files.
constexpr std::string_view kLlvmMingw = "llvm-mingw";
constexpr std::string_view kLlvmMingwVersion = "20260922";
constexpr std::string_view kLlvmMingwRelease =
    "https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/";

// Chromium's Debian 11 sysroots: glibc 2.31 with symbols versioned no newer than 2.26, GCC 10's
// libstdc++, and the X11, ALSA and PulseAudio headers SDL needs. Debian packages, so free to
// redistribute; the checksums are Chromium's (build/linux/sysroot_scripts/sysroots.json).
constexpr std::string_view kSysrootVersion = "bullseye";
constexpr std::string_view kSysrootStore = "https://commondatastorage.googleapis.com/"
                                           "chrome-linux-sysroot/";

std::string sysroot_name(std::string_view arch) {
    return "linux-sysroot-" + std::string(arch);
}

class Sha256 {
public:
    void update(const unsigned char* data, std::size_t size) {
        length_ += size;
        while (size > 0) {
            const std::size_t take = std::min(size, buffer_.size() - buffered_);
            std::copy_n(data, take, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
            buffered_ += take;
            data += take;
            size -= take;
            if (buffered_ == buffer_.size()) {
                compress();
                buffered_ = 0;
            }
        }
    }

    std::string hex() {
        const std::uint64_t bits = length_ * 8;
        const unsigned char one = 0x80;
        update(&one, 1);
        const unsigned char zero = 0;
        while (buffered_ != kLengthOffset) {
            update(&zero, 1);
        }
        std::array<unsigned char, 8> length{};
        for (std::size_t i = 0; i < length.size(); ++i) {
            length.at(i) = static_cast<unsigned char>(bits >> (56 - 8 * i));
        }
        update(length.data(), length.size());
        constexpr std::string_view kDigits = "0123456789abcdef";
        std::string text;
        for (const std::uint32_t word : state_) {
            for (int shift = 28; shift >= 0; shift -= 4) {
                text += kDigits[(word >> shift) & 0xFU];
            }
        }
        return text;
    }

private:
    static constexpr std::size_t kLengthOffset = 56;
    static constexpr std::array<std::uint32_t, 64> kRound{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};

    void compress() {
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

    std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<unsigned char, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0;
};

// The last lines of a failed tool's output, for the error message.
std::string tail_of(const Result<platform::ProcessResult>& result) {
    if (!result) {
        return result.error().message;
    }
    std::string tail = result.value().tail;
    while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r')) {
        tail.pop_back();
    }
    return tail;
}

} // namespace

std::vector<Toolchain> host_toolchains() {
    std::vector<Toolchain> toolchains;
#if defined(__APPLE__) || (defined(__linux__) && defined(__x86_64__))
#if defined(__APPLE__)
    constexpr std::string_view kLlvmMingwHost = "macos-universal";
    constexpr std::string_view kLlvmMingwSha256 =
        "52e5f5a7b131021d0c39a37a38fa380a1da7885cd04bd61afd0cd4ecfb8bc1f3";
#else
    constexpr std::string_view kLlvmMingwHost = "ubuntu-22.04-x86_64";
    constexpr std::string_view kLlvmMingwSha256 =
        "bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21";
#endif
    const std::string folder =
        "llvm-mingw-" + std::string(kLlvmMingwVersion) + "-ucrt-" + std::string(kLlvmMingwHost);
    toolchains.push_back({std::string(kLlvmMingw), std::string(kLlvmMingwVersion),
                          "Clang and LLD with mingw-w64, for Windows and Linux exports",
                          std::string(kLlvmMingwRelease) + folder + ".tar.xz",
                          std::string(kLlvmMingwSha256), folder});
    const std::array<std::pair<std::string_view, std::string_view>, 2> sysroots{{
        {kLinuxX86_64, "aa2b968052fa851240b70e941de1b680459b8b19ab10763077ad13e625f81d22"},
        {kLinuxArm64, "63b2bcd2a1800a143d51590784bb84caa98a695cf65614364968a8be9cd31173"},
    }};
    for (const auto& [arch, sha256] : sysroots) {
        toolchains.push_back(
            {sysroot_name(arch),
             std::string(kSysrootVersion),
             "Debian 11 sysroot (glibc 2.31) for " + std::string(arch) + " Linux exports",
             std::string(kSysrootStore) + std::string(sha256),
             std::string(sha256),
             {}});
    }
#endif
    return toolchains;
}

std::vector<std::string> toolchains_for(std::string_view platform, std::string_view arch) {
#ifdef _WIN32
    (void)platform;
    (void)arch;
    return {};
#else
    if (platform == "windows") {
        return {std::string(kLlvmMingw)};
    }
    if (platform == "linux") {
        return {std::string(kLlvmMingw), sysroot_name(arch)};
    }
    return {};
#endif
}

fs::path toolchains_dir() {
    if (const std::string dir = environment(kToolchainsDirVariable); !dir.empty()) {
        return dir;
    }
#ifdef _WIN32
    const std::string home = environment("USERPROFILE");
#else
    const std::string home = environment("HOME");
#endif
    return fs::path(home) / ".slash" / "toolchains";
}

std::optional<fs::path> installed_toolchain(const Toolchain& toolchain, const fs::path& dir) {
    fs::path folder = dir / (toolchain.name + "-" + toolchain.version);
    std::error_code error;
    if (fs::is_regular_file(folder / kMarker, error)) {
        return folder;
    }
    return std::nullopt;
}

Result<fs::path> install_toolchain(const Toolchain& toolchain, const fs::path& dir,
                                   const std::string& log_path) {
    if (auto installed = installed_toolchain(toolchain, dir)) {
        return *installed;
    }
    std::error_code error;
    const fs::path target = fs::absolute(dir / (toolchain.name + "-" + toolchain.version), error);
    // A unique staging folder next to the target, so the final rename stays on one disk and two
    // installs at once don't collide.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path staging =
        fs::absolute(dir / (".staging-" + toolchain.name + "-" + std::to_string(stamp)), error);
    const fs::path archive = staging / "download";
    const fs::path unpacked = staging / "unpacked";
    fs::create_directories(unpacked, error);
    if (error) {
        return make_error(ErrorCode::PlatformFailure,
                          "could not create " + detail::display(unpacked) + ": " + error.message());
    }
    const auto clean_up = [&] {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
    };

    platform::ProcessOptions process;
    process.log_path = log_path;
    process.tail_lines = 5;
    auto download =
        platform::run_process({"curl", "--fail", "--location", "--silent", "--show-error",
                               "--retry", "3", "--output", archive.string(), toolchain.url},
                              process);
    if (!download || download.value().exit_code != 0) {
        clean_up();
        return make_error(ErrorCode::PlatformFailure, "could not download " + toolchain.name +
                                                          " from " + toolchain.url + ": " +
                                                          tail_of(download));
    }
    auto sha256 = sha256_file(archive);
    if (!sha256) {
        clean_up();
        return sha256.error();
    }
    if (sha256.value() != toolchain.sha256) {
        clean_up();
        return make_error(ErrorCode::InvalidArgument, "the " + toolchain.name + " download from " +
                                                          toolchain.url + " has SHA-256 " +
                                                          sha256.value() + ", not the pinned " +
                                                          toolchain.sha256 + "; it was deleted");
    }
    auto unpack =
        platform::run_process({"tar", "-xf", archive.string(), "-C", unpacked.string()}, process);
    if (!unpack || unpack.value().exit_code != 0) {
        clean_up();
        return make_error(ErrorCode::PlatformFailure,
                          "could not unpack " + toolchain.name + ": " + tail_of(unpack));
    }
    const fs::path root = toolchain.folder.empty() ? unpacked : unpacked / toolchain.folder;
    if (!fs::is_directory(root, error)) {
        clean_up();
        return make_error(ErrorCode::PlatformFailure, "the " + toolchain.name + " archive has no " +
                                                          toolchain.folder + " folder");
    }
    json::Value marker;
    marker["name"] = toolchain.name;
    marker["version"] = toolchain.version;
    marker["sha256"] = toolchain.sha256;
    marker["url"] = toolchain.url;
    if (auto written = platform::write_file((root / kMarker).string(), json::write(marker));
        !written) {
        clean_up();
        return written.error();
    }
    fs::remove_all(target, error);
    fs::rename(root, target, error);
    clean_up();
    if (error && !installed_toolchain(toolchain, dir)) {
        return make_error(ErrorCode::PlatformFailure, "could not move " + toolchain.name +
                                                          " into " + detail::display(target) +
                                                          ": " + error.message());
    }
    return target;
}

std::string sha256_hex(std::string_view data) {
    Sha256 hash;
    hash.update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
    return hash.hex();
}

Result<std::string> sha256_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return make_error(ErrorCode::PlatformFailure, "could not open " + detail::display(file));
    }
    Sha256 hash;
    std::vector<char> chunk(1 << 16);
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        hash.update(reinterpret_cast<const unsigned char*>(chunk.data()),
                    static_cast<std::size_t>(in.gcount()));
    }
    if (in.bad()) {
        return make_error(ErrorCode::PlatformFailure, "could not read " + detail::display(file));
    }
    return hash.hex();
}

} // namespace slash::exporter
