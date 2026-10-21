#include "slash/exporter/toolchains.hpp"

#include "steps.hpp"

#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/core/sha256.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"

#include <algorithm>
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

#ifdef SLASH_WAYLAND_SCANNER
// Wayland's scanner.c, its main() renamed (cmake/Dependencies.cmake).
extern "C" int slash_wayland_scanner_main(int argc, char** argv);
#endif

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

// emsdk's release builds, and the Node.js and Python builds emsdk installs with them, all from
// its bucket. kEmscriptenRelease names the build of a release (emsdk's
// emscripten-releases-tags.json); the checksums are ours, since emsdk checks none.
constexpr std::string_view kEmscriptenVersion = "6.0.10";
constexpr std::string_view kEmscriptenRelease = "666337b525e673e769121856d175f6f52b8ead64";
constexpr std::string_view kEmscriptenBuilds =
    "https://storage.googleapis.com/webassembly/emscripten-releases-builds/";
constexpr std::string_view kNodeVersion = "22.16.0";
constexpr std::string_view kPythonVersion = "3.13.3";

// A build of each for this host: its archive's name and SHA-256, or nothing where emsdk has none.
struct WebBuild {
    std::string_view os; // emsdk's name for it
    std::string_view emscripten;
    std::string_view emscripten_sha256;
    std::string_view node;
    std::string_view node_sha256;
    std::string_view python;
    std::string_view python_sha256;
};

constexpr WebBuild web_build() {
#if defined(__APPLE__) && defined(__aarch64__)
    return {"mac",
            "wasm-binaries-arm64.tar.xz",
            "084896c728d82e44d7aa54afc4a7bf64031dda83f3a4c47ee3164cece247cd76",
            "node-v22.16.0-darwin-arm64.tar.gz",
            "1d7f34ec4c03e12d8b33481e5c4560432d7dc31a0ef3ff5a4d9a8ada7cf6ecc9",
            "python-3.13.3-0-macos-arm64.tar.gz",
            "2b0899d7ade9463b0c909ef23aeea27546a4f412637998d817a49718bc2bac19"};
#elif defined(__APPLE__) && defined(__x86_64__)
    return {"mac",
            "wasm-binaries.tar.xz",
            "8944f69fe334d1b0c470cc37b4fbeb90260c37c4377a9d5c687ce3d8ff1e44fb",
            "node-v22.16.0-darwin-x64.tar.gz",
            "838d400f7e66c804e5d11e2ecb61d6e9e878611146baff69d6a2def3cc23f4ac",
            "python-3.13.3-0-macos-x86_64.tar.gz",
            "d3f30e1b78354d6f74fae951175dc2bd22b386aafeab13edd6ac7c9613b0d33c"};
#elif defined(__linux__) && defined(__x86_64__)
    return {"linux",
            "wasm-binaries.tar.xz",
            "a51dd2829bdf725d268974728e032533b106ea01cb482a62288ec12627a7abbe",
            "node-v22.16.0-linux-x64.tar.xz",
            "f4cb75bb036f0d0eddf6b79d9596df1aaab9ddccd6a20bf489be5abe9467e84e",
            {},
            {}};
#elif defined(__linux__) && defined(__aarch64__)
    return {"linux",
            "wasm-binaries-arm64.tar.xz",
            "cbc9961dda38d0544e6420787339da4792048a04259054d1e4cd4e85c198baf3",
            "node-v22.16.0-linux-arm64.tar.xz",
            "eab80cb88f8fda1e65f5e8d0420c9809bdb320b03fd34976ab7161b6e703b910",
            {},
            {}};
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    // Windows' tar unpacks zip.
    return {"win",
            "wasm-binaries.zip",
            "6de0bf6bad8214a89dedafb5db3cdcfbdf8c395e421ba257e83cb21b77030931",
            "node-v22.16.0-win-x64.zip",
            "c2d6eb9645ffaa3d95665a3cfad49ab206e3cac26099ec23f5d17738abd01c40",
            "python-3.13.3-0-win-amd64.zip",
            "6fe7a540c6b8b185780467cf7495e884ee62316ec4abb19e1c735b8a77c62465"};
#else
    return {};
#endif
}

// An archive's name without its extension, which is the folder it holds.
std::string_view stem(std::string_view archive) {
    return archive.substr(0, std::min(archive.find(".tar"), archive.find(".zip")));
}

void add_web_toolchains(std::vector<Toolchain>& toolchains) {
    constexpr WebBuild kBuild = web_build();
    if (kBuild.emscripten.empty()) {
        return;
    }
    toolchains.push_back({std::string(kEmscripten), std::string(kEmscriptenVersion),
                          "Emscripten with its Clang and Binaryen, for web exports",
                          std::string(kEmscriptenBuilds) + std::string(kBuild.os) + "/" +
                              std::string(kEmscriptenRelease) + "/" +
                              std::string(kBuild.emscripten),
                          std::string(kBuild.emscripten_sha256), "install"});
    toolchains.push_back({std::string(kNode), std::string(kNodeVersion),
                          "Node.js, which Emscripten runs on and web exports are tested with",
                          std::string(kEmscriptenBuilds) + "deps/" + std::string(kBuild.node),
                          std::string(kBuild.node_sha256), std::string(stem(kBuild.node))});
    if (!kBuild.python.empty()) {
#ifdef _WIN32
        const std::string folder; // python.exe sits at the root
#else
        const std::string folder = "python-" + std::string(kPythonVersion) + "-0";
#endif
        toolchains.push_back({std::string(kPython), std::string(kPythonVersion),
                              "Python, which runs Emscripten's tools",
                              std::string(kEmscriptenBuilds) + "deps/" + std::string(kBuild.python),
                              std::string(kBuild.python_sha256), folder});
    }
}

// Eclipse Temurin's JDK and Google's Android command-line tools, whose sdkmanager installs the
// SDK packages. The JDK's checksums are Adoptium's and the command-line tools' are Google's
// (developer.android.com/studio).
constexpr std::string_view kJdkVersion = "21.0.12.1+1";
constexpr std::string_view kJdkRelease =
    "https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.12.1%2B1/";
constexpr std::string_view kCommandLineToolsVersion = "15859902";
constexpr std::string_view kAndroidRepository = "https://dl.google.com/android/repository/";

// Written last into <dir>/android-sdk, naming the packages sdkmanager installed.
constexpr std::string_view kAndroidMarker = ".slash-android-sdk.json";

struct AndroidBuild {
    std::string_view jdk;
    std::string_view jdk_sha256;
    std::string_view tools;
    std::string_view tools_sha256;
};

constexpr AndroidBuild android_build() {
#if defined(__APPLE__) && defined(__aarch64__)
    return {"OpenJDK21U-jdk_aarch64_mac_hotspot_21.0.12.1_1.tar.gz",
            "3623232f33a9c3baadf304480b2535f9a3cba8a58d42ecbb438ba267315d9998",
            "commandlinetools-mac_arm64-15859902_latest.zip",
            "835b62a26162b229b441d1f6d4680383815a270809eb33522c0d480fa5002c4e"};
#elif defined(__APPLE__) && defined(__x86_64__)
    return {"OpenJDK21U-jdk_x64_mac_hotspot_21.0.12.1_1.tar.gz",
            "44db0f08196daf19a47f90d13388b0c943b67663cb537f998fe29e836fa842ce",
            "commandlinetools-mac_x86_64-15859902_latest.zip",
            "c5a6378ab5cf7e0d5701921405115befff13e9ff7417fb588389338f8bd050f3"};
#elif defined(__linux__) && defined(__x86_64__)
    return {"OpenJDK21U-jdk_x64_linux_hotspot_21.0.12.1_1.tar.gz",
            "ce79869e1307ed8ee1e2baa86a412b1eb5b75d10a01006d788a6f968bcfaee94",
            "commandlinetools-linux-15859902_latest.zip",
            "4e4c464f145a7512b57d088ac6c278c03c9eea610886b35a5e0804e74eedf583"};
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return {"OpenJDK21U-jdk_x64_windows_hotspot_21.0.12.1_1.zip",
            "f9d6e191ab098c0d416e7d588a24420a8621cd2f4720dab2459b8b7b2d2d8b4e",
            "commandlinetools-win-15859902_latest.zip",
            "90ae805d20434428bffcb699c290860f19bb5f66a67e6b330067e3de801fb04a"};
#else
    return {};
#endif
}

// A JDK's home: its folder, or Contents/Home in a macOS one.
fs::path jdk_home(const fs::path& folder) {
#ifdef __APPLE__
    return folder / "Contents" / "Home";
#else
    return folder;
#endif
}

// Whether sdkmanager installed every one of packages into sdk.
bool android_packages_installed(const fs::path& sdk, const std::vector<std::string>& packages) {
    auto text = platform::read_file((sdk / kAndroidMarker).string());
    if (!text) {
        return false;
    }
    auto marker = json::parse(text.value());
    if (!marker) {
        return false;
    }
    const json::Value* installed = marker.value().find("packages");
    if (installed == nullptr || !installed->is_array()) {
        return false;
    }
    return std::ranges::all_of(packages, [installed](const std::string& package) {
        return std::ranges::any_of(installed->as_array(), [&package](const json::Value& item) {
            return item.is_string() && item.as_string() == package;
        });
    });
}

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
    add_web_toolchains(toolchains);
    return toolchains;
}

std::optional<AndroidInstall> android_install() {
    constexpr AndroidBuild kBuild = android_build();
    if (kBuild.jdk.empty()) {
        return std::nullopt;
    }
    AndroidInstall install;
    install.jdk = {std::string(kJdk),
                   std::string(kJdkVersion),
                   "Eclipse Temurin's JDK, for Android exports (GPL 2.0 with the Classpath "
                   "Exception, run as a separate program)",
                   std::string(kJdkRelease) + std::string(kBuild.jdk),
                   std::string(kBuild.jdk_sha256),
                   "jdk-" + std::string(kJdkVersion)};
    install.command_line_tools = {std::string(kAndroidCommandLineTools),
                                  std::string(kCommandLineToolsVersion),
                                  "Google's Android command-line tools, whose sdkmanager installs "
                                  "the Android SDK",
                                  std::string(kAndroidRepository) + std::string(kBuild.tools),
                                  std::string(kBuild.tools_sha256),
                                  "cmdline-tools"};
    // What exports need: adb, the build tools, the platform they target, the NDK and the SDK's
    // CMake, which brings Ninja.
    install.packages = {"platform-tools", "build-tools;36.0.0", "platforms;android-36",
                        "ndk;27.3.13750724", "cmake;3.31.6"};
    return install;
}

AndroidFolders installed_android(const AndroidInstall& install, const fs::path& dir) {
    AndroidFolders folders;
    if (const auto jdk = installed_toolchain(install.jdk, dir)) {
        folders.java_home = jdk_home(*jdk);
    }
    if (android_packages_installed(dir / kAndroidSdk, install.packages)) {
        folders.sdk = dir / kAndroidSdk;
    }
    return folders;
}

Result<AndroidFolders> install_android(const AndroidInstall& install, const fs::path& dir,
                                       bool accept_license, const std::string& log_path) {
    if (!accept_license) {
        return make_error(ErrorCode::InvalidArgument,
                          "the Android SDK is under Google's Android Software Development Kit "
                          "License Agreement (" +
                              std::string(kAndroidSdkLicense) +
                              "), which must be accepted before it is installed");
    }
    auto jdk = install_toolchain(install.jdk, dir, log_path);
    if (!jdk) {
        return jdk.error();
    }
    auto tools = install_toolchain(install.command_line_tools, dir, log_path);
    if (!tools) {
        return tools.error();
    }
    std::error_code error;
    AndroidFolders folders{fs::absolute(dir / kAndroidSdk, error), jdk_home(jdk.value())};
    if (android_packages_installed(folders.sdk, install.packages)) {
        return folders;
    }
    fs::create_directories(folders.sdk, error);
    if (error) {
        return make_error(ErrorCode::PlatformFailure, "could not create " +
                                                          detail::display(folders.sdk) + ": " +
                                                          error.message());
    }
    // One package per line, which keeps the ';' in their names away from cmd.exe on Windows.
    const fs::path package_file = folders.sdk / ".slash-packages.txt";
    std::string packages;
    for (const std::string& package : install.packages) {
        packages += package + "\n";
    }
    if (auto written = platform::write_file(package_file.string(), packages); !written) {
        return written.error();
    }
#ifdef _WIN32
    const fs::path sdkmanager = tools.value() / "bin" / "sdkmanager.bat";
#else
    const fs::path sdkmanager = tools.value() / "bin" / "sdkmanager";
#endif
    platform::ProcessOptions process;
    process.log_path = log_path;
    process.tail_lines = 10;
    process.environment = {{"JAVA_HOME", folders.java_home.string()}};
    // sdkmanager shows each license the packages are under that it has no record of accepting
    // and asks; the caller has accepted them.
    for (int i = 0; i < 16; ++i) {
        process.input += "y\n";
    }
    auto installed =
        platform::run_process({sdkmanager.string(), "--sdk_root=" + folders.sdk.string(),
                               "--package_file=" + package_file.string()},
                              process);
    fs::remove(package_file, error);
    if (!installed || installed.value().exit_code != 0) {
        return make_error(ErrorCode::PlatformFailure,
                          "sdkmanager could not install the Android SDK: " + tail_of(installed));
    }
    json::Value marker;
    json::Array names;
    for (const std::string& package : install.packages) {
        names.emplace_back(package);
    }
    marker["packages"] = std::move(names);
    marker["command_line_tools"] = install.command_line_tools.version;
    if (auto written =
            platform::write_file((folders.sdk / kAndroidMarker).string(), json::write(marker));
        !written) {
        return written.error();
    }
    return folders;
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
    if (platform == "web") {
        std::vector<std::string> names{std::string(kEmscripten), std::string(kNode)};
#if defined(__APPLE__) || defined(_WIN32)
        names.emplace_back(kPython);
#endif
        return names;
    }
#ifdef _WIN32
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

WebTools web_tools(const std::vector<fs::path>& folders) {
    WebTools tools;
    const fs::path& emscripten = folders.at(0);
    tools.toolchain_file =
        emscripten / "emscripten" / "cmake" / "Modules" / "Platform" / "Emscripten.cmake";
#ifdef _WIN32
    tools.node = folders.at(1) / "bin" / "node.exe";
    if (folders.size() > 2) {
        tools.python = folders[2] / "python.exe";
    }
#else
    tools.node = folders.at(1) / "bin" / "node";
    if (folders.size() > 2) {
        tools.python = folders[2] / "bin" / "python3";
    }
#endif
    // Emscripten reads its config as Python.
    const auto quoted = [](const fs::path& path) {
        std::string text = "'";
        for (const char c : path.generic_string()) {
            if (c == '\\' || c == '\'') {
                text.push_back('\\');
            }
            text.push_back(c);
        }
        return text + "'";
    };
    tools.config = "LLVM_ROOT = " + quoted(emscripten / "bin") + "\n" +
                   "BINARYEN_ROOT = " + quoted(emscripten) + "\n" +
                   "NODE_JS = " + quoted(tools.node) + "\n";
    return tools;
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
#ifdef __linux__
    // GNU tar can't unpack zip files; CMake's tar, which is libarchive, can.
    const bool zip = toolchain.url.ends_with(".zip");
#else
    const bool zip = false; // macOS's and Windows' tar are bsdtar, which unpacks them
#endif
    platform::ProcessOptions unpack_process = process;
    if (zip) {
        unpack_process.working_directory = unpacked.string();
    }
    auto unpack =
        zip ? platform::run_process({"cmake", "-E", "tar", "xf", archive.string()}, unpack_process)
            : platform::run_process({"tar", "-xf", archive.string(), "-C", unpacked.string()},
                                    process);
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

bool has_wayland_scanner() {
#ifdef SLASH_WAYLAND_SCANNER
    return true;
#else
    return false;
#endif
}

int run_wayland_scanner(int argc, char** argv) {
#ifdef SLASH_WAYLAND_SCANNER
    return slash_wayland_scanner_main(argc, argv);
#else
    (void)argc;
    (void)argv;
    log::error("this build of the engine's tools has no wayland-scanner");
    return 1;
#endif
}

} // namespace slash::exporter
