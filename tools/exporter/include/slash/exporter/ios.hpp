#pragma once

// Helpers for iOS exports that don't need a Mac, so they can be tested anywhere.

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace slash::exporter {

// The oldest iOS that exported games run on (docs/roadmap.md).
inline constexpr std::string_view kIosDeploymentTarget = "14.0";

struct Simulator {
    std::string udid;
    std::string name;
    bool booted = false;
};

// The Simulator to test on, from `xcrun simctl list devices available --json`: a booted iPhone,
// else an iPhone on the newest iOS runtime. Runtimes older than iOS 14 don't count.
[[nodiscard]] std::optional<Simulator> pick_simulator(const json::Value& list);

struct CodesignIdentity {
    std::string hash; // SHA-1, which codesign accepts unambiguously
    std::string name; // e.g. "Apple Distribution: Example Inc (ABCDE12345)"
};

// The codesign identity to sign with, from the output of `security find-identity -v -p
// codesigning`. wanted is a full name, a SHA-1, or a kind such as "Apple Distribution", which
// matches that kind's identities; when several match, the one whose name ends in "(<team>)"
// wins. The error lists the candidates.
[[nodiscard]] Result<CodesignIdentity>
find_codesign_identity(std::string_view identities, std::string_view wanted, std::string_view team);

// Whether a provisioning profile's application-identifier, such as "ABCDE12345.com.example.*",
// covers bundle_id for team.
[[nodiscard]] bool profile_covers(std::string_view app_id, std::string_view team,
                                  std::string_view bundle_id);

} // namespace slash::exporter
