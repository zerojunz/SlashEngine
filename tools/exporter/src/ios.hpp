#pragma once

// The iOS half of an export: signing the app bundle that export/CMakeLists.txt installs,
// zipping device builds into an .ipa and testing Simulator builds. Used by export.cpp only.

#include "steps.hpp"

#include "slash/core/json.hpp"
#include "slash/exporter/presets.hpp"
#include "slash/platform/process.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace slash::exporter::detail {

// What an app is signed with. Simulator builds, and device builds whose preset asks for it, are
// signed ad hoc, with no provisioning profile.
struct IosSigning {
    bool device = false;
    std::string identity = std::string(kAdHocIdentity); // SHA-1 of the keychain identity
    std::string identity_name;
    std::filesystem::path profile;      // the .mobileprovision to embed
    std::string profile_name;           // its Name
    std::filesystem::path entitlements; // the profile's entitlements, for codesign
};

// Checks that Xcode's iOS SDK is installed and finds the identity and provisioning profile for a
// device build, before anything is built, so a missing one fails right away. The profile's
// entitlements are written into work.
bool prepare_ios(const IosOptions& ios, const std::string& bundle_id, bool debug,
                 const std::filesystem::path& work, IosSigning& signing, Failure& failure);

// CMake arguments that build export/CMakeLists.txt for a device or the Simulator.
[[nodiscard]] std::vector<std::string> ios_configure_args(const IosOptions& ios, bool debug,
                                                          const std::string& bundle_id);

// Compiles the app icon's asset catalog (slash/exporter/icons.hpp) into the app with Xcode's
// actool, for a destination's iPhones and iPads, and adds the keys it names to Info.plist.
bool compile_ios_icons(const std::filesystem::path& catalog, const std::filesystem::path& app,
                       std::string_view destination, const std::filesystem::path& work,
                       const platform::ProcessOptions& process, Failure& failure);

// Signs the app in place, embedding the provisioning profile if there is one.
bool sign_ios_app(const IosSigning& signing, const std::filesystem::path& app,
                  const platform::ProcessOptions& process, Failure& failure);

// Zips the app into an .ipa, as App Store Connect and device installs expect.
bool package_ipa(const std::filesystem::path& app, const std::filesystem::path& ipa,
                 const std::filesystem::path& work, const platform::ProcessOptions& process,
                 Failure& failure);

// Installs a Simulator build on a Simulator, booting one if none is running, runs it with
// --headless --ticks 60 and reads its summary. Skipped when no Simulator is installed.
[[nodiscard]] json::Value test_ios(const std::filesystem::path& app, const std::string& bundle_id,
                                   const std::filesystem::path& work,
                                   const std::filesystem::path& log_path);

} // namespace slash::exporter::detail
