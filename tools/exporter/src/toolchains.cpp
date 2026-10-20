#include "slash/exporter/toolchains.hpp"

#include "steps.hpp"

#include "slash/core/json.hpp"
#include "slash/core/sha256.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <optional>
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

std::optional<Toolchain> assetripper() {
    constexpr std::string_view kVersion = "2.0.0";
    constexpr std::string_view kRelease =
        "https://github.com/AssetRipper/AssetRipper/releases/download/2.0.0/";
#if defined(__APPLE__) && defined(__aarch64__)
    constexpr std::string_view kArchive = "AssetRipper_mac_arm64.tar.xz";
    constexpr std::string_view kSha256 =
        "237a6da404bf089c5e5c71a5a9a698c2ea580872292dfa5ce21e25fd1dbe9045";
#elif defined(__APPLE__) && defined(__x86_64__)
    constexpr std::string_view kArchive = "AssetRipper_mac_x64.tar.xz";
    constexpr std::string_view kSha256 =
        "1b3836f135d16845cb57595fadff5739cbb86d28caaf621005b4301dba9b5a56";
#elif defined(__linux__) && defined(__x86_64__)
    constexpr std::string_view kArchive = "AssetRipper_linux_x64.tar.xz";
    constexpr std::string_view kSha256 =
        "6f5d4352d36795b944243eb3ec790ec37dbca6d6756e52605bc8616e042d384e";
#elif defined(__linux__) && defined(__aarch64__)
    constexpr std::string_view kArchive = "AssetRipper_linux_arm64.tar.xz";
    constexpr std::string_view kSha256 =
        "8031752621d7fb27addab6108bad8bfbd43546bbb07b36912a987dfb4029c914";
#elif defined(_WIN32) && defined(_M_ARM64)
    constexpr std::string_view kArchive = "AssetRipper_win_arm64.zip"; // Windows' tar unpacks zip
    constexpr std::string_view kSha256 =
        "c2c3a335948aa0ecaf26baf22ac77e36993d1a1695b5db291cb63f66872627f1";
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::string_view kArchive = "AssetRipper_win_x64.zip";
    constexpr std::string_view kSha256 =
        "9a7ef0e7c5c3ea5b90b4e6d855e2d98d5f7ec8c3f9e26fccbc194c6a7b01baf7";
#else
#define SLASH_NO_ASSETRIPPER
#endif
#ifdef SLASH_NO_ASSETRIPPER
    (void)kVersion;
    (void)kRelease;
    return std::nullopt;
#else
    return Toolchain{std::string(kAssetRipper),
                     std::string(kVersion),
                     "AssetRipper, which turns Unity builds back into projects for import (GPL "
                     "3.0, run as a separate program)",
                     std::string(kRelease) + std::string(kArchive),
                     std::string(kSha256),
                     {}};
#endif
}

std::string_view assetripper_program() {
#ifdef _WIN32
    return "AssetRipper.GUI.Free.exe";
#else
    return "AssetRipper.GUI.Free";
#endif
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
    return slash::sha256_hex(data);
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
        hash.update({chunk.data(), static_cast<std::size_t>(in.gcount())});
    }
    if (in.bad()) {
        return make_error(ErrorCode::PlatformFailure, "could not read " + detail::display(file));
    }
    return hash.hex();
}

} // namespace slash::exporter
