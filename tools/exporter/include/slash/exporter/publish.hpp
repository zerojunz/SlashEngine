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
//               "branch": "beta"},
//     "epic": {"organization": "o-abc", "product": "<product id>",
//              "artifacts": {"windows": "<artifact id>", "macos": "<artifact id>"},
//              "label": "Live", "sandbox": "<sandbox id>"},
//     "gog": {"product": "1234567890", "presets": ["windows", "mac-release"],
//             "languages": ["en-US"], "branch": "staging"}
//   }
// Each channel or depot key is a preset, or a platform for its defaults. Without "channels",
// every preset itch.io can take is published, on a channel named after its platform; Steam's
// depots and Epic's artifacts are always listed, since their ids are the game's own. Uploads run
// the store's own tool as a program of its own (itch.io's butler, Steam's steamcmd, Epic's
// BuildPatchTool, GOG's Pipeline Builder), which reads its credentials from the environment or its
// own login cache: nothing secret goes in the file.
namespace slash::exporter {

// The stores publish knows.
inline const std::vector<std::string_view> kStores{"itch", "steam", "epic", "gog"};

// The environment variables itch.io's butler reads: its API key, and where butler is when it
// isn't on PATH.
inline constexpr std::string_view kButlerApiKey = "BUTLER_API_KEY";
inline constexpr std::string_view kButlerPath = "SLASH_BUTLER_PATH";

// The Steam account steamcmd signs in as, and where steamcmd is when it isn't on PATH. steamcmd
// keeps the account's login once it has been signed in by hand (Steam Guard asks then), so no
// password is passed.
inline constexpr std::string_view kSteamUser = "STEAM_USERNAME";
inline constexpr std::string_view kSteamcmdPath = "SLASH_STEAMCMD_PATH";

// The Epic Games Store's BuildPatchTool client, from the product's BuildPatchTool Credentials
// page, and where BuildPatchTool is when it isn't on PATH. The secret is passed by its variable's
// name, so it never shows in a command line.
inline constexpr std::string_view kEpicClientId = "EPIC_CLIENT_ID";
inline constexpr std::string_view kEpicClientSecret = "EPIC_CLIENT_SECRET";
inline constexpr std::string_view kBuildPatchToolPath = "SLASH_BUILDPATCHTOOL_PATH";

// The GOG account Pipeline Builder signs in as, the password of the private branch builds go to
// when it has one, and where Pipeline Builder is when it isn't on PATH. Tool output is reported
// with the passwords taken out.
inline constexpr std::string_view kGogUser = "GOG_USERNAME";
inline constexpr std::string_view kGogPassword = "GOG_PASSWORD";
inline constexpr std::string_view kGogBranchPassword = "GOG_BRANCH_PASSWORD";
inline constexpr std::string_view kGogPipelineBuilderPath = "SLASH_GOG_PIPELINE_BUILDER_PATH";

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

struct EpicStore {
    std::string organization; // the organization id
    std::string product;      // the game's product id
    // Artifact ids by preset or platform name, in name order.
    std::map<std::string, std::string, std::less<>> artifacts;
    std::string label;   // the label set on each upload, such as "Live"; empty sets none
    std::string sandbox; // the sandbox id the label is set in
};

struct GogStore {
    std::string product;                         // the game's product id, digits
    std::vector<std::string> presets;            // presets or platforms; empty means all GOG takes
    std::vector<std::string> languages{"en-US"}; // the depot's languages
    std::string branch; // the private branch builds are published to; empty leaves it
};

struct Stores {
    std::optional<ItchStore> itch;
    std::optional<SteamStore> steam;
    std::optional<EpicStore> epic;
    std::optional<GogStore> gog;
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

// GOG's name for a platform whose builds it takes, "windows" or "osx"; "" for the others.
[[nodiscard]] std::string_view gog_platform(std::string_view platform);

// The Epic Games Store's name for a platform whose builds it takes, "Windows" or "Mac"; "" for
// the others.
[[nodiscard]] std::string_view epic_platform(std::string_view platform);

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
    // uploaded. epic: {"store", "organization", "product", "label", "sandbox", "version",
    // "upload", "builds": [{"preset", "platform", "artifact", "build_version", "output",
    // "app_launch", "commands"} plus "export" and "push" once uploaded]}. gog: {"store",
    // "product", "branch", "version", "upload", "builds": [{"preset", "platform", "output",
    // "project": {"path", "json"}, "command"} plus "export" and "push" once uploaded]}. A plan
    // adds "needs": what an upload takes. On failure: {"error": {"step", "message", ...}} and
    // "pushed", the targets uploaded before it. A step of "options" means the request or the
    // presets were wrong, and "setup" that this machine lacks the store's tool or its
    // credentials.
    json::Value json;
};

// Publishes a game's exports to a store: works out each build's preset and channel, then, when
// uploading, exports each one and pushes its output with the store's tool. Stops at the first
// build that fails, saying which were pushed before it.
[[nodiscard]] PublishResult publish_game(const PublishOptions& options);

} // namespace slash::exporter
