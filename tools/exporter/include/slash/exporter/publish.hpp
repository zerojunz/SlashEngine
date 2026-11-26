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
//              "channels": {"mac-release": "mac", "windows": "windows"}}
//   }
// Each channel key is a preset, or a platform for its defaults. Without "channels", every preset
// a store can take is published, on a channel named after its platform. Uploads run the store's
// own tool as a program of its own (itch.io's butler), which reads its credentials from the
// environment: nothing secret goes in the file.
namespace slash::exporter {

// The stores publish knows.
inline const std::vector<std::string_view> kStores{"itch"};

// The environment variables itch.io's butler reads: its API key, and where butler is when it
// isn't on PATH.
inline constexpr std::string_view kButlerApiKey = "BUTLER_API_KEY";
inline constexpr std::string_view kButlerPath = "SLASH_BUTLER_PATH";

struct ItchStore {
    std::string project; // "user/game", as in the game's itch.io address
    // Channels by preset or platform name, in name order; empty means every preset itch.io takes.
    std::map<std::string, std::string, std::less<>> channels;
};

struct Stores {
    std::optional<ItchStore> itch;
};

// The JSON Schema of export_presets.json's "stores".
[[nodiscard]] json::Value stores_schema();

// Checks what the schema can't in a parsed export_presets.json's "stores": that each channel
// names a preset or a platform, and not an iOS one. validate_presets includes these.
[[nodiscard]] std::vector<json::ValidationError> validate_stores(const json::Value& presets);

// Reads a validated export_presets.json's "stores".
[[nodiscard]] Stores parse_stores(const json::Value& presets);

// The itch.io channel a platform's builds go to when "channels" doesn't name one: "mac",
// "windows", "linux" (or "linux-arm64"), "html5" or "android". butler tags the channel's
// downloads by these names. iOS games can't be published on itch.io, so "".
[[nodiscard]] std::string itch_channel(std::string_view platform, std::string_view arch = {});

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
    // {"store", "project", "version", "upload", "builds": [{"preset", "platform", "channel",
    // "target", "output", "command"} plus "export" and "push" once uploaded]} and, for a plan,
    // "needs": what an upload takes. On failure: {"error": {"step", "message", ...}} and
    // "pushed", the targets uploaded before it. A step of "options" means the request or the
    // presets were wrong, and "setup" that this machine lacks butler or its key.
    json::Value json;
};

// Publishes a game's exports to a store: works out each build's preset and channel, then, when
// uploading, exports each one and pushes its output with the store's tool. Stops at the first
// build that fails, saying which were pushed before it.
[[nodiscard]] PublishResult publish_game(const PublishOptions& options);

} // namespace slash::exporter
