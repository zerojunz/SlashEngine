#pragma once

// The Android half of an export: building the APK from what export/CMakeLists.txt installs, and
// testing it on a device. Used by export.cpp only.

#include "steps.hpp"

#include "slash/core/json.hpp"
#include "slash/exporter/android.hpp"
#include "slash/platform/process.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace slash::exporter::detail {

// The key an APK is signed with.
struct AndroidKey {
    bool release = false;
    std::string keystore;
    // apksigner's key arguments. Passwords are passed as env:<variable>, never as text, so they
    // stay off command lines and out of logs.
    std::vector<std::string> signer_args;
};

// Release builds are signed with the keystore in SLASH_ANDROID_KEYSTORE_PATH when it is set; the
// rest with the debug keystore in ~/.android, as Android Studio does, created when first needed.
bool prepare_android_key(bool debug, AndroidKey& key, Failure& failure);

// Options for running Android tools: JAVA_HOME and PATH point at the JDK.
[[nodiscard]] platform::ProcessOptions android_process(const AndroidTools& tools,
                                                       platform::ProcessOptions process);

// Ninja for the NDK build: from PATH, else from the SDK's CMake. "" if there is none.
[[nodiscard]] std::string android_ninja(const AndroidTools& tools);

// CMake arguments that build export/CMakeLists.txt for one ABI.
[[nodiscard]] std::vector<std::string> android_configure_args(const AndroidTools& tools,
                                                              const std::string& abi,
                                                              const std::string& package);

struct ApkInfo {
    std::string package;
    std::int64_t version_code = 1;
    std::string version_name;
    bool debug = false;
};

// Compiles the Java sources in tree/java, then links, aligns and signs tree's manifest, assets,
// libraries and code into apk. Scratch files go in work.
bool package_apk(const AndroidTools& tools, const AndroidKey& key, const ApkInfo& info,
                 const std::filesystem::path& tree, const std::filesystem::path& work,
                 const std::filesystem::path& apk, const platform::ProcessOptions& process,
                 Failure& failure);

// Installs the APK on the connected device or emulator (ANDROID_SERIAL picks one), runs it with
// --headless --ticks 60 and reads its summary from logcat. Skipped when no device is connected
// or the device's ABIs aren't in the APK.
[[nodiscard]] json::Value test_apk(const AndroidTools& tools, const std::filesystem::path& apk,
                                   const std::string& package, const std::vector<std::string>& abis,
                                   const std::filesystem::path& log_path);

} // namespace slash::exporter::detail
