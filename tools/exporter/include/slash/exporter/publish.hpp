#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Publishing exports to stores. export_presets.json's "stores" says where each store's builds
// go:
//   "stores": {
//     "itch": {"project": "player/space-game",
//              "channels": {"mac-release": "mac", "windows": "windows"}},
//     "steam": {"app": "1234560", "depots": {"windows": "1234561", "macos": "1234562"},
//               "branch": "beta"}
//   }
// Each channel or depot key is a preset, or a platform for its defaults. Without "channels",
// every preset itch.io can take is published, on a channel named after its platform; Steam's
// depots are always listed, since their ids are the game's own. Uploads run the store's own tool
// as a program of its own (itch.io's butler, Steam's steamcmd), which reads its credentials from
// the environment or its own login cache: nothing secret goes in the file.
namespace slash::exporter {

// The stores publish knows.
inline const std::vector<std::string_view> kStores{"itch", "steam"};

// The environment variables itch.io's butler reads: its API key, and where butler is when it
// isn't on PATH.
inline constexpr std::string_view kButlerApiKey = "BUTLER_API_KEY";
inline constexpr std::string_view kButlerPath = "SLASH_BUTLER_PATH";

// The Steam account steamcmd signs in as, and where steamcmd is when it isn't on PATH. steamcmd
// keeps the account's login once it has been signed in by hand (Steam Guard asks then), so no
// password is passed.
inline constexpr std::string_view kSteamUser = "STEAM_USERNAME";
inline constexpr std::string_view kSteamcmdPath = "SLASH_STEAMCMD_PATH";

struct ItchStore {
    std::string project; // "user/game", as in the game's itch.io address
    // Channels by preset or platform name, in name order; empty means every preset itch.io takes.
    std::map<std::string, std::string, std::less<>> channels;
};

struct SteamStore {
    std::string app; // the game's app id, digits
    // Depot ids by preset or platform name, in name order.
    std::map<std::string, std::string, std::less<>> depots;
    std::string branch; // the branch the build is set live on; empty leaves it for Steamworks
};

struct Stores {
    std::optional<ItchStore> itch;
    std::optional<SteamStore> steam;
};

// The JSON Schema of export_presets.json's "stores".
[[nodiscard]] json::Value stores_schema();

// Checks what the schema can't in a parsed export_presets.json's "stores": the names' forms, and
// that each channel or depot names a preset or a platform the store takes. validate_presets
// includes these.
[[nodiscard]] std::vector<json::ValidationError> validate_stores(const json::Value& presets);

// Reads a validated export_presets.json's "stores".
[[nodiscard]] Stores parse_stores(const json::Value& presets);

// The itch.io channel a platform's builds go to when "channels" doesn't name one: "mac",
// "windows", "linux" (or "linux-arm64"), "html5" or "android". butler tags the channel's
// downloads by these names. iOS games can't be published on itch.io, so "".
[[nodiscard]] std::string itch_channel(std::string_view platform, std::string_view arch = {});

// Whether Steam takes a platform's builds: Windows, macOS and Linux.
[[nodiscard]] bool steam_takes(std::string_view platform);

struct PublishOptions {
    std::string store;         // one of kStores
    std::string project = "."; // game folder with project.json and export_presets.json
    // Presets or platforms to publish; empty means the store's channels.
    std::vector<std::string> targets;
    // Upload the builds. False returns the plan: what would be built and pushed, and where.
    bool upload = false;
    // Export each preset first. False pushes the output an earlier export left.
    bool build = true;
    std::string engine;  // engine folder exports build from; default: found automatically
    std::string version; // the version stores show; default: project.json's
};

struct PublishResult {
    bool ok = false;
    // itch: {"store", "project", "version", "upload", "builds": [{"preset", "platform",
    // "channel", "target", "output", "command"} plus "export" and "push" once uploaded]}.
    // steam: {"store", "app", "branch", "version", "upload", "script", "command", "builds":
    // [{"preset", "platform", "depot", "output"} plus "export" once uploaded]} and "push" once
    // uploaded. A plan adds "needs": what an upload takes. On failure: {"error": {"step",
    // "message", ...}} and "pushed", the targets uploaded before it. A step of "options" means the
    // request or the presets were wrong, and "setup" that this machine lacks butler or its key.
    json::Value json;
};

// Publishes a game's exports to a store: works out each build's preset and channel, then, when
// uploading, exports each one and pushes its output with the store's tool. Stops at the first
// build that fails, saying which were pushed before it.
[[nodiscard]] PublishResult publish_game(const PublishOptions& options);

} // namespace slash::exporter
