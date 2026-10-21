#include "ios.hpp"

#include "slash/core/log.hpp"
#include "slash/exporter/ios.hpp"
#include "slash/platform/file.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace slash::exporter {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kRuntimePrefix = "SimRuntime.iOS-";
constexpr int kMinimumIosMajor = 14;
constexpr std::size_t kHashLength = 40;
constexpr auto kTestTimeout = std::chrono::seconds(120);

#if defined(__x86_64__)
constexpr std::string_view kHostArchitecture = "x86_64";
#else
constexpr std::string_view kHostArchitecture = "arm64";
#endif

std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

// Runs a tool and returns its trimmed output, or nullopt if it failed.
std::optional<std::string> run_tool(const std::vector<std::string>& args, int lines = 1) {
    platform::ProcessOptions process;
    process.tail_lines = lines;
    auto result = platform::run_process(args, process);
    if (!result || result.value().exit_code != 0) {
        return std::nullopt;
    }
    return trimmed(std::move(result).value().tail);
}

// The last count lines of text.
std::string last_lines(const std::string& text, std::size_t count) {
    std::size_t start = text.size();
    for (std::size_t seen = 0; start > 0 && seen <= count;) {
        --start;
        if (text[start] == '\n') {
            ++seen;
        }
    }
    return trimmed(text.substr(start == 0 ? 0 : start + 1));
}

// "com.apple.CoreSimulator.SimRuntime.iOS-17-5" -> {17, 5}; empty for other runtimes.
std::vector<int> runtime_version(std::string_view key) {
    const std::size_t at = key.find(kRuntimePrefix);
    if (at == std::string_view::npos) {
        return {};
    }
    std::vector<int> parts;
    int value = 0;
    bool digits = false;
    for (const char c : key.substr(at + kRuntimePrefix.size())) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            digits = true;
        } else if (c == '-' && digits) {
            parts.push_back(value);
            value = 0;
            digits = false;
        } else {
            return {};
        }
    }
    if (digits) {
        parts.push_back(value);
    }
    return parts;
}

std::string string_field(const json::Value& object, std::string_view key) {
    const json::Value* value = object.find(key);
    return value != nullptr && value->is_string() ? value->as_string() : std::string();
}

// The current time as ExpirationDate appears in provisioning profiles: 2026-10-14T09:30:00Z.
std::string now_utc() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::array<char, 32> text{};
    const std::size_t size = std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return {text.data(), size};
}

struct Profile {
    fs::path file;
    fs::path plist; // the decoded profile
    std::string name;
    std::string team;
    std::string app_id;
    std::string expires;
    bool debuggable = false; // get-task-allow: development profiles allow debuggers
};

// Decodes a .mobileprovision into plist and reads the fields signing needs.
std::optional<Profile> read_profile(const fs::path& file, const fs::path& plist) {
    std::error_code error;
    fs::create_directories(plist.parent_path(), error);
    if (!run_tool({"security", "cms", "-D", "-i", file.string(), "-o", plist.string()})) {
        return std::nullopt;
    }
    const auto extract = [&](const std::string& key) {
        return run_tool({"plutil", "-extract", key, "raw", "-o", "-", plist.string()}).value_or("");
    };
    Profile profile;
    profile.file = file;
    profile.plist = plist;
    profile.name = extract("Name");
    profile.team = extract("TeamIdentifier.0");
    profile.app_id = extract("Entitlements.application-identifier");
    profile.expires = extract("ExpirationDate");
    profile.debuggable = extract("Entitlements.get-task-allow") == "true";
    return profile;
}

} // namespace

std::optional<Simulator> pick_simulator(const json::Value& list) {
    const json::Value* devices = list.is_object() ? list.find("devices") : nullptr;
    if (devices == nullptr || !devices->is_object()) {
        return std::nullopt;
    }
    std::optional<Simulator> best;
    std::vector<int> best_version;
    for (const auto& [runtime, entries] : devices->as_object()) {
        const std::vector<int> version = runtime_version(runtime);
        if (version.empty() || version.front() < kMinimumIosMajor || !entries.is_array()) {
            continue;
        }
        for (const json::Value& device : entries.as_array()) {
            if (!device.is_object()) {
                continue;
            }
            const std::string name = string_field(device, "name");
            const std::string type = string_field(device, "deviceTypeIdentifier");
            const json::Value* available = device.find("isAvailable");
            Simulator candidate{string_field(device, "udid"), name,
                                string_field(device, "state") == "Booted"};
            if (candidate.udid.empty() ||
                (type.find("iPhone") == std::string::npos && !name.starts_with("iPhone")) ||
                (available != nullptr && available->is_bool() && !available->as_bool())) {
                continue;
            }
            if (candidate.booted) {
                return candidate;
            }
            if (!best || version > best_version) {
                best = std::move(candidate);
                best_version = version;
            }
        }
    }
    return best;
}

Result<CodesignIdentity> find_codesign_identity(std::string_view identities,
                                                std::string_view wanted, std::string_view team) {
    // Lines look like:   1) 0123456789ABCDEF0123456789ABCDEF01234567 "Apple Distribution: ..."
    std::vector<CodesignIdentity> matches;
    std::string_view rest = identities;
    while (!rest.empty()) {
        const std::size_t end = std::min(rest.find('\n'), rest.size());
        const std::string_view line = rest.substr(0, end);
        rest.remove_prefix(std::min(end + 1, rest.size()));
        const std::size_t number = line.find(") ");
        if (number == std::string_view::npos) {
            continue;
        }
        const std::string_view entry = line.substr(number + 2);
        const std::size_t open = entry.find(" \"");
        const std::size_t close = entry.rfind('"');
        if (open != kHashLength || close == std::string_view::npos || close <= open + 1) {
            continue;
        }
        CodesignIdentity identity{std::string(entry.substr(0, open)),
                                  std::string(entry.substr(open + 2, close - open - 2))};
        const bool named = identity.name == wanted || identity.hash == wanted ||
                           identity.name.starts_with(std::string(wanted) + ":");
        const bool seen = std::ranges::any_of(
            matches, [&](const CodesignIdentity& match) { return match.hash == identity.hash; });
        if (named && !seen) {
            matches.push_back(std::move(identity));
        }
    }
    if (matches.size() == 1) {
        return matches.front();
    }
    if (matches.empty()) {
        return make_error(ErrorCode::NotFound,
                          "no \"" + std::string(wanted) +
                              "\" codesign identity is in the keychain; create one in Xcode "
                              "(Settings > Accounts > Manage Certificates) or import its .p12, "
                              "then check with \"security find-identity -v -p codesigning\"");
    }
    const std::string suffix = "(" + std::string(team) + ")";
    std::vector<CodesignIdentity> for_team;
    std::ranges::copy_if(matches, std::back_inserter(for_team), [&](const CodesignIdentity& match) {
        return match.name.ends_with(suffix);
    });
    if (for_team.size() == 1) {
        return for_team.front();
    }
    std::string names;
    for (const CodesignIdentity& match : matches) {
        names += (names.empty() ? "\"" : ", \"") + match.name + "\"";
    }
    return make_error(ErrorCode::InvalidArgument,
                      "several codesign identities match \"" + std::string(wanted) + "\": " +
                          names + "; set codesign_identity in the preset's ios section to one");
}

bool profile_covers(std::string_view app_id, std::string_view team, std::string_view bundle_id) {
    if (team.empty() || app_id.size() <= team.size() || !app_id.starts_with(team) ||
        app_id[team.size()] != '.') {
        return false;
    }
    const std::string_view pattern = app_id.substr(team.size() + 1);
    if (pattern.ends_with('*')) {
        return bundle_id.starts_with(pattern.substr(0, pattern.size() - 1));
    }
    return pattern == bundle_id;
}

namespace detail {

bool prepare_ios(const IosOptions& ios, const std::string& bundle_id, bool debug,
                 const fs::path& work, IosSigning& signing, Failure& failure) {
    signing = IosSigning{};
    const std::string sdk = ios.destination == kIosDevice ? "iphoneos" : "iphonesimulator";
    if (!run_tool({"xcrun", "--sdk", sdk, "--show-sdk-path"})) {
        failure = {"options",
                   "iOS exports need Xcode with its iOS SDK; install Xcode and select it with "
                   "\"sudo xcode-select -s /Applications/Xcode.app\"",
                   {}};
        return false;
    }
    if (ios.destination != kIosDevice) {
        return true;
    }
    signing.device = true;
    // An ad hoc .ipa needs no team or profile, and installs nowhere until it is re-signed.
    if (ios.codesign_identity == kAdHocIdentity) {
        return true;
    }
    std::string wanted = ios.codesign_identity;
    if (wanted.empty()) {
        wanted = debug ? "Apple Development" : "Apple Distribution";
    }
    const std::optional<std::string> identities =
        run_tool({"security", "find-identity", "-v", "-p", "codesigning"}, 500);
    auto identity = find_codesign_identity(identities.value_or(""), wanted, ios.team_id);
    if (!identity) {
        failure = {"options", identity.error().message, {}};
        return false;
    }
    signing.identity = identity.value().hash;
    signing.identity_name = identity.value().name;

    std::error_code error;
    const fs::path profiles = fs::absolute(work / "profiles", error);
    fs::remove_all(profiles, error);
    const std::string now = now_utc();
    const std::string exact_id = ios.team_id + "." + bundle_id;
    std::optional<Profile> chosen;
    if (const std::string path = environment(kIosProvisioningProfile); !path.empty()) {
        chosen = read_profile(path, profiles / "chosen.plist");
        std::string problem;
        if (!chosen) {
            problem = "is not a provisioning profile";
        } else if (chosen->team != ios.team_id) {
            problem = "is for team " + chosen->team + ", not " + ios.team_id;
        } else if (!profile_covers(chosen->app_id, ios.team_id, bundle_id)) {
            problem = "covers " + chosen->app_id + ", not " + exact_id;
        } else if (chosen->expires <= now) {
            problem = "expired on " + chosen->expires;
        }
        if (!problem.empty()) {
            failure = {"options",
                       std::string(kIosProvisioningProfile) + " names " + path + ", which " +
                           problem,
                       {}};
            return false;
        }
    } else {
        // The profiles Xcode downloaded: development ones for debug builds, distribution ones
        // for release. Exact bundle IDs beat wildcards, then the one that lasts longest.
        const fs::path home = environment("HOME");
        int index = 0;
        for (const fs::path& folder :
             {home / "Library" / "MobileDevice" / "Provisioning Profiles",
              home / "Library" / "Developer" / "Xcode" / "UserData" / "Provisioning Profiles"}) {
            for (auto it = fs::directory_iterator(folder, error);
                 !error && it != fs::directory_iterator(); it.increment(error)) {
                if (it->path().extension() != ".mobileprovision") {
                    continue;
                }
                std::optional<Profile> profile =
                    read_profile(it->path(), profiles / (std::to_string(index++) + ".plist"));
                if (!profile || profile->team != ios.team_id ||
                    !profile_covers(profile->app_id, ios.team_id, bundle_id) ||
                    profile->expires <= now || profile->debuggable != debug) {
                    continue;
                }
                const bool exact = profile->app_id == exact_id;
                const bool chosen_exact = chosen && chosen->app_id == exact_id;
                if (!chosen || (exact && !chosen_exact) ||
                    (exact == chosen_exact && profile->expires > chosen->expires)) {
                    chosen = std::move(profile);
                }
            }
            error.clear();
        }
    }
    if (!chosen) {
        failure = {"options",
                   std::string("no ") + (debug ? "development" : "distribution") +
                       " provisioning profile for team " + ios.team_id + " covers " + bundle_id +
                       "; download one in Xcode (Settings > Accounts > Download Manual "
                       "Profiles) or from developer.apple.com and set " +
                       std::string(kIosProvisioningProfile) + " to its path",
                   {}};
        return false;
    }
    signing.profile = chosen->file;
    signing.profile_name = chosen->name;
    // The profile's entitlements, with a wildcard app ID made concrete, as Xcode does.
    signing.entitlements = profiles / "entitlements.plist";
    if (!run_tool({"plutil", "-extract", "Entitlements", "xml1", "-o",
                   signing.entitlements.string(), chosen->plist.string()}) ||
        !run_tool({"plutil", "-replace", "application-identifier", "-string", exact_id,
                   signing.entitlements.string()})) {
        failure = {"options", "could not read the entitlements in " + display(chosen->file), {}};
        return false;
    }
    return true;
}

std::vector<std::string> ios_configure_args(const IosOptions& ios, bool debug,
                                            const std::string& bundle_id) {
    const bool simulator = ios.destination == kIosSimulator;
    // Release builds run in the Simulator on Apple silicon and Intel Macs; debug builds only on
    // this one.
    std::string architectures = "arm64";
    if (simulator) {
        architectures = debug ? std::string(kHostArchitecture) : "arm64;x86_64";
    }
    return {
        "-DCMAKE_SYSTEM_NAME=iOS",
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=" + std::string(kIosDeploymentTarget),
        std::string("-DCMAKE_OSX_SYSROOT=") + (simulator ? "iphonesimulator" : "iphoneos"),
        "-DCMAKE_OSX_ARCHITECTURES=" + architectures,
        "-DSLASH_GAME_ID=" + bundle_id,
        "-DSLASH_IOS_BUILD=" + (ios.build_number > 0 ? std::to_string(ios.build_number) : ""),
    };
}

bool compile_ios_icons(const fs::path& catalog, const fs::path& app, std::string_view destination,
                       const fs::path& work, const platform::ProcessOptions& process,
                       Failure& failure) {
    std::error_code error;
    // actool writes Assets.car and the icons older iOS versions read, and the Info.plist keys
    // that name them, which PlistBuddy merges in.
    const std::string partial = fs::absolute(work / "icons.plist", error).string();
    return step("icons",
                {"xcrun",
                 "actool",
                 fs::absolute(catalog, error).string(),
                 "--compile",
                 fs::absolute(app, error).string(),
                 "--platform",
                 destination == kIosDevice ? "iphoneos" : "iphonesimulator",
                 "--minimum-deployment-target",
                 std::string(kIosDeploymentTarget),
                 "--app-icon",
                 "AppIcon",
                 "--target-device",
                 "iphone",
                 "--target-device",
                 "ipad",
                 "--output-partial-info-plist",
                 partial,
                 "--errors",
                 "--warnings",
                 "--output-format",
                 "human-readable-text"},
                process, failure) &&
           step("icons",
                {"/usr/libexec/PlistBuddy", "-c", "Merge " + partial,
                 fs::absolute(app / "Info.plist", error).string()},
                process, failure);
}

bool sign_ios_app(const IosSigning& signing, const fs::path& app,
                  const platform::ProcessOptions& process, Failure& failure) {
    std::error_code error;
    const std::string bundle = fs::absolute(app, error).string();
    std::vector<std::string> args{"codesign", "--force", "--sign", signing.identity,
                                  "--timestamp=none"};
    if (!signing.profile.empty()) {
        fs::copy_file(signing.profile, app / "embedded.mobileprovision",
                      fs::copy_options::overwrite_existing, error);
        if (error) {
            failure = {"sign", "could not embed the provisioning profile: " + error.message(), {}};
            return false;
        }
        args.insert(args.end(), {"--entitlements", signing.entitlements.string(),
                                 "--generate-entitlement-der"});
    }
    args.push_back(bundle);
    // What SDK packages embed is signed first, with the same identity and no entitlements.
    if (!sign_nested(app / "Frameworks",
                     {"codesign", "--force", "--sign", signing.identity, "--timestamp=none"},
                     process, failure)) {
        return false;
    }
    return step("sign", args, process, failure) &&
           step("sign", {"codesign", "--verify", "--strict", bundle}, process, failure);
}

bool package_ipa(const fs::path& app, const fs::path& ipa, const fs::path& work,
                 const platform::ProcessOptions& process, Failure& failure) {
    std::error_code error;
    const fs::path payload = fs::absolute(work / "ipa" / "Payload", error);
    fs::remove_all(payload.parent_path(), error);
    fs::create_directories(payload, error);
    if (error) {
        failure = {"package", "could not create " + display(payload) + ": " + error.message(), {}};
        return false;
    }
    return step("package",
                {"ditto", fs::absolute(app, error).string(), (payload / app.filename()).string()},
                process, failure) &&
           step("package",
                {"ditto", "-c", "-k", "--norsrc", "--keepParent", payload.string(),
                 fs::absolute(ipa, error).string()},
                process, failure);
}

json::Value test_ios(const fs::path& app, const std::string& bundle_id, const fs::path& work,
                     const fs::path& log_path) {
    json::Value test;
    const std::optional<std::string> list =
        run_tool({"xcrun", "simctl", "list", "devices", "available", "--json"}, 100000);
    if (!list) {
        test["skipped"] = "xcrun simctl is not available; install Xcode";
        return test;
    }
    auto parsed = json::parse(*list);
    const std::optional<Simulator> simulator =
        parsed ? pick_simulator(parsed.value()) : std::nullopt;
    if (!simulator) {
        test["skipped"] = "no iPhone Simulator with iOS 14 or newer is installed; add one in "
                          "Xcode (Settings > Components)";
        return test;
    }
    test["device"] = simulator->name;

    log::info("export step", {{"step", "test"}});
    platform::ProcessOptions process;
    process.log_path = log_path.string();
    process.tail_lines = 20;
    const auto run = [&](const std::vector<std::string>& args) {
        return platform::run_process(args, process);
    };
    const auto succeeded = [](const Result<platform::ProcessResult>& result) {
        return result && result.value().exit_code == 0;
    };
    const auto failed = [&](const std::string& message,
                            const Result<platform::ProcessResult>& result) {
        test["passed"] = false;
        test["message"] = message;
        if (result) {
            test["tail"] = result.value().tail;
        }
        return test;
    };
    // bootstatus -b boots the Simulator if needed and waits until it is ready.
    if (!simulator->booted) {
        auto booted = run({"xcrun", "simctl", "bootstatus", simulator->udid, "-b"});
        if (!succeeded(booted)) {
            return failed("could not boot the " + simulator->name + " Simulator", booted);
        }
    }
    const auto shut_down = [&] {
        if (!simulator->booted) {
            (void)run({"xcrun", "simctl", "shutdown", simulator->udid});
        }
    };
    std::error_code error;
    auto installed =
        run({"xcrun", "simctl", "install", simulator->udid, fs::absolute(app, error).string()});
    if (!succeeded(installed)) {
        shut_down();
        return failed("could not install the app in the Simulator", installed);
    }
    const fs::path out = fs::absolute(work / "test-stdout.log", error);
    const fs::path err = fs::absolute(work / "test-stderr.log", error);
    fs::remove(out, error);
    fs::remove(err, error);
    auto launched = run({"xcrun", "simctl", "launch", "--terminate-running-process",
                         "--stdout=" + out.string(), "--stderr=" + err.string(), simulator->udid,
                         bundle_id, "--headless", "--ticks", std::to_string(kTestTicks)});
    if (!succeeded(launched)) {
        shut_down();
        return failed("could not launch the app in the Simulator", launched);
    }
    // simctl prints "<bundle id>: <pid>". Simulator apps are processes of this Mac, so ps sees
    // whether the game is still running.
    const std::string started_line = trimmed(launched.value().tail);
    const std::string pid = started_line.substr(started_line.find_last_of(' ') + 1);

    const auto read_summary = [&] {
        auto text = platform::read_file(out.string());
        return text ? find_summary(text.value()) : std::nullopt;
    };
    const auto launched_at = std::chrono::steady_clock::now();
    std::optional<json::Value> summary;
    while (std::chrono::steady_clock::now() < launched_at + kTestTimeout) {
        summary = read_summary();
        if (summary) {
            break;
        }
        if (!run_tool({"ps", "-p", pid})) {
            // One more look, in case the game printed its summary just before exiting.
            summary = read_summary();
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    shut_down();

    const bool passed = summary.has_value() && summary->find("ticks")->is_int() &&
                        summary->find("ticks")->as_int() == static_cast<std::int64_t>(kTestTicks);
    test["passed"] = passed;
    if (summary) {
        add_summary(test, *summary);
    }
    if (!passed) {
        test["message"] = summary ? "the game stopped early"
                                  : "the game exited or timed out without printing its summary";
        auto errors = platform::read_file(err.string());
        test["tail"] = errors ? last_lines(errors.value(), 20) : std::string();
    }
    return test;
}

} // namespace detail
} // namespace slash::exporter
