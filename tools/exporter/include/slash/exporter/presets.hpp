#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Export presets live in export_presets.json at the project root, like Godot's
// export_presets.cfg. Each preset names a platform and how to build and sign for it:
//
//   {
//     "format": 1,
//     "presets": {
//       "mac-release": {
//         "platform": "macos",
//         "macos": {"codesign_identity": "Developer ID Application: Example (ABCDE12345)",
//                   "notarize": true}
//       }
//     }
//   }
//
// Secrets never go in the file. Passwords and keys come from environment variables, listed in
// the schema's descriptions, so the file can be committed and edited by agents.
namespace slash::exporter {

inline constexpr std::string_view kPresetsFile = "export_presets.json";
inline constexpr std::int64_t kPresetsFormat = 1;

// The platforms export knows, in the order usage text lists them.
inline const std::vector<std::string_view> kPlatforms{"macos", "windows", "linux", "web"};

// Environment variables that hold signing secrets.
inline constexpr std::string_view kAppleApiKeyPath = "SLASH_APPLE_API_KEY_PATH";
inline constexpr std::string_view kAppleApiKeyId = "SLASH_APPLE_API_KEY_ID";
inline constexpr std::string_view kAppleApiIssuer = "SLASH_APPLE_API_ISSUER";
inline constexpr std::string_view kWindowsCertificatePath = "SLASH_WINDOWS_CERTIFICATE_PATH";
inline constexpr std::string_view kWindowsCertificatePassword =
    "SLASH_WINDOWS_CERTIFICATE_PASSWORD";

inline constexpr std::string_view kAdHocIdentity = "-";
inline constexpr std::string_view kDefaultTimestampUrl = "http://timestamp.digicert.com";

struct MacosOptions {
    // codesign identity from the keychain; "-" signs ad hoc, which runs only on the Mac that
    // built the game or after the player allows it.
    std::string codesign_identity = std::string(kAdHocIdentity);
    std::string entitlements; // project-relative .plist; optional
    bool notarize = false;    // submit to Apple's notary service and staple the ticket
};

struct WindowsOptions {
    bool sign = false; // sign with the certificate in SLASH_WINDOWS_CERTIFICATE_PATH
    std::string timestamp_url = std::string(kDefaultTimestampUrl); // "" for no timestamp
};

struct Preset {
    std::string name;
    std::string platform;
    std::string output; // project-relative; "" means build/export/<name>
    bool debug = false;
    MacosOptions macos;
    WindowsOptions windows;
};

using Presets = std::map<std::string, Preset, std::less<>>;

// The JSON Schema for export_presets.json.
[[nodiscard]] json::Value presets_schema();

// Checks a parsed export_presets.json against the schema and the rules it can't express:
// preset names, project-relative paths, platform-specific sections and signing settings.
// Returns every error.
[[nodiscard]] std::vector<json::ValidationError> validate_presets(const json::Value& presets);

// Reads validated presets. The error names the first problem with its JSON Pointer.
[[nodiscard]] Result<Presets> parse_presets(const json::Value& presets);

// Reads <folder>/export_presets.json. A project without the file has no presets.
[[nodiscard]] Result<Presets> load_presets(std::string_view folder);

// The preset called target, or else the defaults for the platform called target. The error
// suggests the closest preset or platform name.
[[nodiscard]] Result<Preset> find_preset(const Presets& presets, std::string_view target);

} // namespace slash::exporter
