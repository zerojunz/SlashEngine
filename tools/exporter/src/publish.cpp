#include "slash/exporter/publish.hpp"

#include "steps.hpp"

#include "slash/core/text.hpp"
#include "slash/exporter/export.hpp"
#include "slash/exporter/presets.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"
#include "slash/runtime/project.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace slash::exporter {
namespace {

namespace fs = std::filesystem;
using detail::display;

// The longest itch.io project and channel names: generous, but enough to catch a pasted URL.
constexpr std::int64_t kMaxStoreName = 128;

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

// One build a publish pushes: a preset and the channel it goes to.
struct Build {
    Preset preset;
    std::string channel; // itch.io's
    std::string depot;   // Steam's
    fs::path output;     // relative to the project, as presets name it
};

PublishResult fail(std::string step, std::string message, json::Value details = {},
                   json::Array pushed = {}) {
    PublishResult result;
    json::Value& error = result.json["error"];
    if (details.is_object()) {
        error = std::move(details);
    }
    error["step"] = std::move(step);
    error["message"] = std::move(message);
    if (!pushed.empty()) {
        result.json["pushed"] = std::move(pushed);
    }
    return result;
}

// Where an export of the preset puts its game, relative to the project.
fs::path output_of(const Preset& preset) {
    if (!preset.output.empty()) {
        return {preset.output};
    }
    return fs::path("build") / "export" / preset.name;
}

std::string store_names() {
    std::string names;
    for (const std::string_view store : kStores) {
        names += names.empty() ? "" : ", ";
        names += store;
    }
    return names;
}

// The command that pushes a build's output, with butler as its program.
std::vector<std::string> push_command(const std::string& butler, const std::string& project,
                                      const Build& build, const std::string& version,
                                      const fs::path& output) {
    return {butler,          "push",  display(output), project + ":" + build.channel,
            "--userversion", version, "--if-changed"};
}

json::Value command_json(const std::vector<std::string>& command) {
    json::Array out;
    for (const std::string& arg : command) {
        out.emplace_back(arg);
    }
    return out;
}

// The builds a publish to itch.io pushes, in order.
Result<std::vector<Build>> itch_builds(const ItchStore& itch, const Presets& presets,
                                       const std::vector<std::string>& targets) {
    std::vector<std::string> names = targets;
    if (names.empty()) {
        for (const auto& [name, channel] : itch.channels) {
            names.push_back(name);
        }
    }
    if (names.empty()) {
        for (const auto& [name, preset] : presets) {
            if (!itch_channel(preset.platform).empty()) {
                names.push_back(name);
            }
        }
    }
    if (names.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "nothing to publish: name presets or platforms, or list them in " +
                              std::string(kPresetsFile) + "'s stores.itch.channels");
    }
    std::vector<Build> builds;
    std::set<std::string, std::less<>> channels;
    for (const std::string& name : names) {
        auto preset = find_preset(presets, name);
        if (!preset) {
            return preset.error();
        }
        Build build;
        build.preset = std::move(preset).value();
        if (const auto it = itch.channels.find(name); it != itch.channels.end()) {
            build.channel = it->second;
        } else {
            build.channel = itch_channel(build.preset.platform, build.preset.linux_options.arch);
        }
        if (build.channel.empty()) {
            return make_error(ErrorCode::InvalidArgument, "\"" + name + "\" builds for " +
                                                              build.preset.platform +
                                                              ", which itch.io doesn't take");
        }
        if (!channels.insert(build.channel).second) {
            return make_error(ErrorCode::InvalidArgument,
                              "two builds go to the itch.io channel \"" + build.channel +
                                  "\", so the second would replace the first; give \"" + name +
                                  "\" a channel of its own in stores.itch.channels");
        }
        build.output = output_of(build.preset);
        builds.push_back(std::move(build));
    }
    return builds;
}

// butler's program: SLASH_BUTLER_PATH's, else butler on PATH.
Result<std::string> find_butler() {
    if (std::string configured = detail::environment(kButlerPath); !configured.empty()) {
        if (!platform::exists(configured)) {
            return make_error(ErrorCode::NotFound, std::string(kButlerPath) + " names \"" +
                                                       configured + "\", which doesn't exist");
        }
        return configured;
    }
    if (std::string found = detail::find_on_path("butler"); !found.empty()) {
        return found;
    }
    return make_error(ErrorCode::NotFound,
                      "uploading to itch.io needs butler, itch.io's upload tool, which isn't on "
                      "PATH: install it from https://itch.io/docs/butler/, or set " +
                          std::string(kButlerPath) + " to its program");
}

// Whether text is 1 or more of the characters allowed, each a letter, a digit or one of extra.
bool name_of(std::string_view text, std::string_view extra) {
    return !text.empty() && std::ranges::all_of(text, [extra](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               extra.find(c) != std::string_view::npos;
    });
}

// export_presets.json's stores.itch, or null.
const json::Value* itch_section(const json::Value& presets) {
    const json::Value* stores = presets.find("stores");
    return stores == nullptr ? nullptr : stores->find("itch");
}

// What every store's publish reads first: the presets, where builds go, and the version.
struct Loaded {
    Presets presets;
    Stores stores;
    std::string version;
};

// Reads options.project's presets, stores and version into loaded; a failure, or nothing.
std::optional<PublishResult> load(const PublishOptions& options, Loaded& loaded) {
    const fs::path project_dir(options.project);
    const std::string presets_path = (project_dir / std::string(kPresetsFile)).string();
    if (!platform::exists(presets_path)) {
        return fail("options", "publishing needs " + std::string(kPresetsFile) + " with a stores." +
                                   options.store + " section saying where builds go");
    }
    auto text = platform::read_file(presets_path);
    if (!text) {
        return fail("options", text.error().message);
    }
    auto parsed = json::parse(text.value());
    if (!parsed) {
        return fail("options", std::string(kPresetsFile) + ": " + parsed.error().message);
    }
    auto presets = parse_presets(parsed.value());
    if (!presets) {
        return fail("options", presets.error().message);
    }
    loaded.presets = std::move(presets).value();
    loaded.stores = parse_stores(parsed.value());
    loaded.version = options.version;
    if (loaded.version.empty()) {
        auto project = runtime::load_project(options.project);
        if (!project) {
            return fail("options", project.error().message);
        }
        loaded.version = project.value().version;
    }
    return std::nullopt;
}

// Exports a build's preset when options.build is on, recording the export in item, then checks
// its output is there; a failure, or nothing.
std::optional<PublishResult> export_build(const Build& build, const PublishOptions& options,
                                          json::Value& item, json::Array& pushed) {
    if (options.build) {
        ExportOptions export_options;
        export_options.target = build.preset.name;
        export_options.project = options.project;
        export_options.engine = options.engine;
        const ExportResult exported = export_game(export_options);
        if (!exported.ok) {
            json::Value details;
            details["preset"] = build.preset.name;
            if (const json::Value* export_error = exported.json.find("error");
                export_error != nullptr) {
                details["export"] = *export_error;
            }
            return fail("export", "exporting \"" + build.preset.name + "\" failed",
                        std::move(details), std::move(pushed));
        }
        item["export"] = exported.json;
    }
    std::error_code error;
    if (!fs::is_directory(fs::path(options.project) / build.output, error)) {
        json::Value details;
        details["preset"] = build.preset.name;
        return fail("push",
                    "there's no export of \"" + build.preset.name + "\" in " +
                        display(build.output) + ": export it first, or publish with build on",
                    std::move(details), std::move(pushed));
    }
    return std::nullopt;
}

// The folder publish keeps its logs and scripts in, made if missing.
fs::path publish_dir(const PublishOptions& options) {
    const fs::path dir = fs::path(options.project) / ".slash" / "publish";
    std::error_code error;
    fs::create_directories(dir, error);
    return dir;
}

PublishResult publish_itch(const PublishOptions& options, const Loaded& loaded) {
    if (!loaded.stores.itch) {
        return fail("options", std::string(kPresetsFile) +
                                   " has no stores.itch: add {\"project\": \"user/game\"}, the "
                                   "game's itch.io project");
    }
    const ItchStore& itch = *loaded.stores.itch;
    auto builds = itch_builds(itch, loaded.presets, options.targets);
    if (!builds) {
        return fail("options", builds.error().message);
    }
    const std::string& version = loaded.version;

    std::string butler = "butler";
    if (options.upload) {
        auto found = find_butler();
        if (!found) {
            return fail("setup", found.error().message);
        }
        butler = std::move(found).value();
        if (detail::environment(kButlerApiKey).empty()) {
            return fail("setup", "uploading to itch.io needs butler's API key in " +
                                     std::string(kButlerApiKey) +
                                     ": make one at https://itch.io/user/settings/api-keys");
        }
    }

    PublishResult result;
    json::Value& out = result.json;
    out["store"] = options.store;
    out["project"] = itch.project;
    out["version"] = version;
    out["upload"] = options.upload;
    json::Array planned;
    for (const Build& build : builds.value()) {
        json::Value item;
        item["preset"] = build.preset.name;
        item["platform"] = build.preset.platform;
        item["channel"] = build.channel;
        item["target"] = itch.project + ":" + build.channel;
        item["output"] = display(build.output);
        item["command"] =
            command_json(push_command("butler", itch.project, build, version, build.output));
        planned.push_back(std::move(item));
    }
    if (!options.upload) {
        out["builds"] = std::move(planned);
        out["build"] = options.build;
        out["needs"] = json::Array{std::string("butler on PATH or in ") + std::string(kButlerPath),
                                   std::string(kButlerApiKey)};
        result.ok = true;
        return result;
    }

    const fs::path log_dir = publish_dir(options);
    json::Array pushed;
    for (std::size_t i = 0; i < builds.value().size(); ++i) {
        const Build& build = builds.value()[i];
        json::Value& item = planned[i];
        if (std::optional<PublishResult> failed = export_build(build, options, item, pushed)) {
            return std::move(*failed);
        }
        platform::ProcessOptions process;
        process.log_path = (log_dir / (options.store + "-" + build.preset.name + ".log")).string();
        process.tail_lines = 20;
        const std::vector<std::string> command = push_command(
            butler, itch.project, build, version, fs::path(options.project) / build.output);
        auto ran = platform::run_process(command, process);
        if (!ran || ran.value().exit_code != 0) {
            json::Value details;
            details["preset"] = build.preset.name;
            details["target"] = item.find("target")->as_string();
            details["log"] = display(fs::path(process.log_path));
            std::string message;
            if (!ran) {
                message = "butler couldn't be started: " + ran.error().message;
            } else {
                details["exit_code"] = ran.value().exit_code;
                details["tail"] = ran.value().tail;
                message = "butler couldn't push \"" + build.preset.name + "\" to " +
                          item.find("target")->as_string();
            }
            return fail("push", std::move(message), std::move(details), std::move(pushed));
        }
        json::Value push;
        push["log"] = display(fs::path(process.log_path));
        push["tail"] = ran.value().tail;
        item["push"] = std::move(push);
        pushed.emplace_back(item.find("target")->as_string());
    }
    out["builds"] = std::move(planned);
    result.ok = true;
    return result;
}

// The builds a publish to Steam uploads, in order, each with its depot.
Result<std::vector<Build>> steam_builds(const SteamStore& steam, const Presets& presets,
                                        const std::vector<std::string>& targets) {
    std::vector<std::string> names = targets;
    if (names.empty()) {
        for (const auto& [name, depot] : steam.depots) {
            names.push_back(name);
        }
    }
    if (names.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "nothing to publish: list the game's depots in " +
                              std::string(kPresetsFile) + "'s stores.steam.depots");
    }
    std::vector<Build> builds;
    std::set<std::string, std::less<>> depots;
    for (const std::string& name : names) {
        auto preset = find_preset(presets, name);
        if (!preset) {
            return preset.error();
        }
        Build build;
        build.preset = std::move(preset).value();
        if (!steam_takes(build.preset.platform)) {
            return make_error(ErrorCode::InvalidArgument, "\"" + name + "\" builds for " +
                                                              build.preset.platform +
                                                              ", which Steam doesn't take");
        }
        for (const std::string& key : {name, build.preset.name, build.preset.platform}) {
            if (const auto it = steam.depots.find(key); it != steam.depots.end()) {
                build.depot = it->second;
                break;
            }
        }
        if (build.depot.empty()) {
            return make_error(ErrorCode::InvalidArgument,
                              "\"" + name +
                                  "\" has no Steam depot: add its depot id to "
                                  "stores.steam.depots, by preset or by platform");
        }
        if (!depots.insert(build.depot).second) {
            return make_error(ErrorCode::InvalidArgument,
                              "two builds go to Steam depot " + build.depot +
                                  ", so the second would replace the first; give \"" + name +
                                  "\" a depot of its own in stores.steam.depots");
        }
        build.output = output_of(build.preset);
        builds.push_back(std::move(build));
    }
    return builds;
}

// steamcmd's program: SLASH_STEAMCMD_PATH's, else steamcmd (or steamcmd.sh) on PATH.
Result<std::string> find_steamcmd() {
    if (std::string configured = detail::environment(kSteamcmdPath); !configured.empty()) {
        if (!platform::exists(configured)) {
            return make_error(ErrorCode::NotFound, std::string(kSteamcmdPath) + " names \"" +
                                                       configured + "\", which doesn't exist");
        }
        return configured;
    }
    for (const char* name : {"steamcmd", "steamcmd.sh"}) {
        if (std::string found = detail::find_on_path(name); !found.empty()) {
            return found;
        }
    }
    return make_error(ErrorCode::NotFound,
                      "uploading to Steam needs steamcmd, Valve's command-line Steam client, which "
                      "isn't on PATH: install it from "
                      "https://developer.valvesoftware.com/wiki/SteamCMD, or set " +
                          std::string(kSteamcmdPath) + " to its program");
}

// text as a quoted VDF string.
std::string vdf_string(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    out += '"';
    return out;
}

// Where publish steam writes its app build script, relative to the project.
fs::path steam_script_path(const SteamStore& steam) {
    return fs::path(".slash") / "publish" / "steam" / ("app_build_" + steam.app + ".vdf");
}

// The app build script steamcmd runs: every build's output, each in its depot. Paths are relative
// to the script, which lives three folders down in the project, so the script reads the same
// wherever the project is.
std::string steam_script(const SteamStore& steam, const std::vector<Build>& builds,
                         const std::string& version) {
    std::string out = "\"AppBuild\"\n{\n";
    out += "\t\"AppID\" " + vdf_string(steam.app) + "\n";
    out += "\t\"Desc\" " + vdf_string(version) + "\n";
    out += "\t\"ContentRoot\" \"../../../\"\n";
    out += "\t\"BuildOutput\" \"output/\"\n";
    if (!steam.branch.empty()) {
        out += "\t\"SetLive\" " + vdf_string(steam.branch) + "\n";
    }
    out += "\t\"Depots\"\n\t{\n";
    for (const Build& build : builds) {
        out += "\t\t" + vdf_string(build.depot) + "\n\t\t{\n";
        out += "\t\t\t\"FileMapping\"\n\t\t\t{\n";
        out += "\t\t\t\t\"LocalPath\" " + vdf_string(build.output.generic_string() + "/*") + "\n";
        out += "\t\t\t\t\"DepotPath\" \".\"\n";
        out += "\t\t\t\t\"recursive\" \"1\"\n";
        out += "\t\t\t}\n\t\t}\n";
    }
    out += "\t}\n}\n";
    return out;
}

// The command that runs a Steam app build script, signed in as user.
std::vector<std::string> steam_command(const std::string& steamcmd, const std::string& user,
                                       const std::string& script) {
    return {steamcmd, "+login", user, "+run_app_build", script, "+quit"};
}

// The build id steamcmd reports on success ("Successfully finished AppID 480 build (BuildID
// 123)."), "" when the output doesn't say the build finished.
std::string steam_build_id(std::string_view output) {
    constexpr std::string_view kFinished = "Successfully finished AppID";
    constexpr std::string_view kBuildId = "(BuildID ";
    const std::size_t finished = output.rfind(kFinished);
    if (finished == std::string_view::npos) {
        return {};
    }
    const std::size_t at = output.find(kBuildId, finished);
    if (at == std::string_view::npos) {
        return "unknown";
    }
    const std::size_t begin = at + kBuildId.size();
    const std::size_t end = output.find(')', begin);
    return std::string(output.substr(begin, end == std::string_view::npos ? end : end - begin));
}

PublishResult publish_steam(const PublishOptions& options, const Loaded& loaded) {
    if (!loaded.stores.steam) {
        return fail("options", std::string(kPresetsFile) +
                                   " has no stores.steam: add {\"app\": \"<app id>\", "
                                   "\"depots\": {\"<preset or platform>\": \"<depot id>\"}}, "
                                   "from the game's Steamworks page");
    }
    const SteamStore& steam = *loaded.stores.steam;
    auto builds = steam_builds(steam, loaded.presets, options.targets);
    if (!builds) {
        return fail("options", builds.error().message);
    }

    std::string steamcmd = "steamcmd";
    std::string user = "$" + std::string(kSteamUser);
    if (options.upload) {
        auto found = find_steamcmd();
        if (!found) {
            return fail("setup", found.error().message);
        }
        steamcmd = std::move(found).value();
        user = detail::environment(kSteamUser);
        if (user.empty()) {
            return fail("setup", "uploading to Steam needs the Steamworks account to sign in as, "
                                 "in " +
                                     std::string(kSteamUser) +
                                     ": an account with Edit App Metadata and Publish App "
                                     "Changes To Steam permissions, signed in once by hand with "
                                     "steamcmd +login so steamcmd keeps its login");
        }
    }

    const fs::path script = steam_script_path(steam);
    const std::string script_text = steam_script(steam, builds.value(), loaded.version);
    PublishResult result;
    json::Value& out = result.json;
    out["store"] = options.store;
    out["app"] = steam.app;
    if (!steam.branch.empty()) {
        out["branch"] = steam.branch;
    }
    out["version"] = loaded.version;
    out["upload"] = options.upload;
    json::Array planned;
    for (const Build& build : builds.value()) {
        json::Value item;
        item["preset"] = build.preset.name;
        item["platform"] = build.preset.platform;
        item["depot"] = build.depot;
        item["output"] = display(build.output);
        planned.push_back(std::move(item));
    }
    out["script"]["path"] = display(script);
    out["script"]["text"] = script_text;
    out["command"] = command_json(steam_command("steamcmd", user, display(script)));
    if (!options.upload) {
        out["builds"] = std::move(planned);
        out["build"] = options.build;
        out["needs"] =
            json::Array{std::string("steamcmd on PATH or in ") + std::string(kSteamcmdPath),
                        std::string(kSteamUser)};
        result.ok = true;
        return result;
    }

    // Steam takes every depot in one app build, so every export comes first.
    json::Array pushed;
    for (std::size_t i = 0; i < builds.value().size(); ++i) {
        if (std::optional<PublishResult> failed =
                export_build(builds.value()[i], options, planned[i], pushed)) {
            return std::move(*failed);
        }
    }
    const fs::path script_file = fs::path(options.project) / script;
    std::error_code error;
    fs::create_directories(script_file.parent_path(), error);
    if (auto written = platform::write_file(script_file.string(), script_text); !written) {
        return fail("push", written.error().message);
    }
    platform::ProcessOptions process;
    process.log_path = (publish_dir(options) / "steam.log").string();
    process.tail_lines = 20;
    fs::remove(process.log_path, error);
    const std::string target = "app " + steam.app;
    auto ran = platform::run_process(
        steam_command(steamcmd, user, fs::absolute(script_file, error).string()), process);
    const std::string build_id = ran ? steam_build_id(ran.value().tail) : std::string();
    if (!ran || ran.value().exit_code != 0 || build_id.empty()) {
        json::Value details;
        details["log"] = display(fs::path(process.log_path));
        std::string message;
        if (!ran) {
            message = "steamcmd couldn't be started: " + ran.error().message;
        } else {
            details["exit_code"] = ran.value().exit_code;
            details["tail"] = ran.value().tail;
            message = "steamcmd couldn't upload the build to Steam " + target;
        }
        return fail("push", std::move(message), std::move(details));
    }
    out["builds"] = std::move(planned);
    out["push"]["build_id"] = build_id;
    out["push"]["log"] = display(fs::path(process.log_path));
    out["push"]["tail"] = ran.value().tail;
    result.ok = true;
    return result;
}

// Whether text is 1 or more digits, as Steam's ids are.
bool digits(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}

// export_presets.json's stores.steam, or null.
const json::Value* steam_section(const json::Value& presets) {
    const json::Value* stores = presets.find("stores");
    return stores == nullptr ? nullptr : stores->find("steam");
}

// The platform a store section's key builds for: its preset's, or the key's when it names a
// platform; nothing when it names neither.
std::optional<std::string> platform_of(const json::Value& presets, const std::string& key) {
    const json::Value* section = presets.find("presets");
    if (const json::Value* preset = section == nullptr ? nullptr : section->find(key);
        preset != nullptr) {
        return preset->find("platform")->as_string();
    }
    if (std::ranges::find(kPlatforms, key) != kPlatforms.end()) {
        return key;
    }
    return std::nullopt;
}

// stores.steam's checks: digit ids, depots naming Windows, macOS or Linux builds, a branch name.
void validate_steam(const json::Value& presets, std::vector<json::ValidationError>& errors) {
    const json::Value* steam = steam_section(presets);
    if (steam == nullptr) {
        return;
    }
    if (const json::Value* app = steam->find("app"); app != nullptr && !digits(app->as_string())) {
        errors.push_back({"/stores/steam/app", "expected the app id's digits, from Steamworks"});
    }
    if (const json::Value* depots = steam->find("depots"); depots != nullptr) {
        for (const auto& [name, depot] : depots->as_object()) {
            const std::string pointer = "/stores/steam/depots/" + json::pointer_escape(name);
            if (!digits(depot.as_string())) {
                errors.push_back({pointer, "expected the depot id's digits, from Steamworks"});
            } else if (const auto platform = platform_of(presets, name); !platform) {
                errors.push_back({pointer, "no preset or platform called \"" + name + "\""});
            } else if (!steam_takes(*platform)) {
                errors.push_back({pointer, "Steam takes Windows, macOS and Linux builds, not " +
                                               *platform + " ones"});
            }
        }
    }
    if (const json::Value* branch = steam->find("branch"); branch != nullptr) {
        if (!name_of(branch->as_string(), "._-")) {
            errors.push_back(
                {"/stores/steam/branch", "branch names are letters, digits, '.', '_' or '-'"});
        } else if (branch->as_string() == "default") {
            errors.push_back({"/stores/steam/branch",
                              "Steam sets the default branch live only from Steamworks: leave "
                              "branch out and set the build live there"});
        }
    }
}

} // namespace

json::Value stores_schema() {
    json::Value project = property("string", "The game's itch.io project as \"user/game\", from "
                                             "its address https://user.itch.io/game.");
    project["maxLength"] = kMaxStoreName;
    json::Value channel = property("string", "Letters, digits, '.', '_' or '-'.");
    channel["maxLength"] = kMaxStoreName;
    json::Value channels =
        property("object", "Channels by preset, or by platform for its defaults, such as "
                           "{\"mac-release\": \"mac\"}. butler tags a channel's downloads by "
                           "the platform in its name. Default: every preset but iOS ones, on a "
                           "channel named after its platform (mac, windows, linux, linux-arm64, "
                           "html5, android).");
    channels["additionalProperties"] = std::move(channel);
    json::Value itch;
    itch["type"] = "object";
    itch["description"] = "Where publish itch sends builds, through butler. butler reads its "
                          "API key from " +
                          std::string(kButlerApiKey) + ".";
    itch["additionalProperties"] = false;
    itch["required"] = json::Array{"project"};
    itch["properties"]["project"] = std::move(project);
    itch["properties"]["channels"] = std::move(channels);

    json::Value app = property("string", "The game's Steam app id, from Steamworks.");
    app["maxLength"] = kMaxStoreName;
    json::Value depot = property("string", "A depot id, from the app's Depots page.");
    depot["maxLength"] = kMaxStoreName;
    json::Value depots = property(
        "object", "Depot ids by preset, or by platform for its presets, such as {\"windows\": "
                  "\"1234561\"}. Steam takes Windows, macOS and Linux builds.");
    depots["additionalProperties"] = std::move(depot);
    depots["minProperties"] = 1;
    json::Value branch =
        property("string", "The branch the build is set live on, such as \"beta\". Steam sets "
                           "the default branch live only from Steamworks. Default: none.");
    branch["maxLength"] = kMaxStoreName;
    json::Value steam;
    steam["type"] = "object";
    steam["description"] = "Where publish steam sends builds, through steamcmd, signed in as " +
                           std::string(kSteamUser) + ".";
    steam["additionalProperties"] = false;
    steam["required"] = json::Array{"app", "depots"};
    steam["properties"]["app"] = std::move(app);
    steam["properties"]["depots"] = std::move(depots);
    steam["properties"]["branch"] = std::move(branch);

    json::Value stores;
    stores["type"] = "object";
    stores["description"] = "Where slash-cli publish sends the presets' builds, by store.";
    stores["additionalProperties"] = false;
    stores["properties"]["itch"] = std::move(itch);
    stores["properties"]["steam"] = std::move(steam);
    return stores;
}

std::string itch_channel(std::string_view platform, std::string_view arch) {
    if (platform == "macos") {
        return "mac";
    }
    if (platform == "windows") {
        return "windows";
    }
    if (platform == "linux") {
        return arch == kLinuxArm64 ? "linux-arm64" : "linux";
    }
    if (platform == "web") {
        return "html5";
    }
    if (platform == "android") {
        return "android";
    }
    return {};
}

bool steam_takes(std::string_view platform) {
    return platform == "windows" || platform == "macos" || platform == "linux";
}

std::vector<json::ValidationError> validate_stores(const json::Value& presets) {
    std::vector<json::ValidationError> errors;
    validate_steam(presets, errors);
    const json::Value* itch = itch_section(presets);
    if (const json::Value* project = itch == nullptr ? nullptr : itch->find("project");
        project != nullptr) {
        const std::string& text = project->as_string();
        const std::size_t slash = text.find('/');
        if (slash == std::string::npos || !name_of(text.substr(0, slash), "_-") ||
            !name_of(text.substr(slash + 1), "_-")) {
            errors.push_back({"/stores/itch/project",
                              "expected \"user/game\", as in the game's address "
                              "https://user.itch.io/game"});
        }
    }
    const json::Value* channels = itch == nullptr ? nullptr : itch->find("channels");
    if (channels == nullptr) {
        return errors;
    }
    for (const auto& [name, channel] : channels->as_object()) {
        const std::string pointer = "/stores/itch/channels/" + json::pointer_escape(name);
        if (!name_of(channel.as_string(), "._-")) {
            errors.push_back({pointer, "channel names are letters, digits, '.', '_' or '-'"});
        } else if (const auto platform = platform_of(presets, name); !platform) {
            errors.push_back({pointer, "no preset or platform called \"" + name + "\""});
        } else if (*platform == "ios") {
            errors.push_back({pointer, "itch.io doesn't take iOS games"});
        }
    }
    return errors;
}

Stores parse_stores(const json::Value& presets) {
    Stores stores;
    if (const json::Value* itch = itch_section(presets); itch != nullptr) {
        ItchStore store;
        store.project = itch->find("project")->as_string();
        if (const json::Value* channels = itch->find("channels"); channels != nullptr) {
            for (const auto& [name, channel] : channels->as_object()) {
                store.channels.emplace(name, channel.as_string());
            }
        }
        stores.itch = std::move(store);
    }
    if (const json::Value* steam = steam_section(presets); steam != nullptr) {
        SteamStore store;
        store.app = steam->find("app")->as_string();
        for (const auto& [name, depot] : steam->find("depots")->as_object()) {
            store.depots.emplace(name, depot.as_string());
        }
        if (const json::Value* branch = steam->find("branch"); branch != nullptr) {
            store.branch = branch->as_string();
        }
        stores.steam = std::move(store);
    }
    return stores;
}

PublishResult publish_game(const PublishOptions& options) {
    if (std::ranges::find(kStores, options.store) == kStores.end()) {
        std::string message = "no store called \"" + options.store + "\"";
        if (const std::string_view hint = closest_match(options.store, kStores); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        } else {
            message += "; publish knows " + store_names();
        }
        return fail("options", std::move(message));
    }
    Loaded loaded;
    if (std::optional<PublishResult> failed = load(options, loaded)) {
        return std::move(*failed);
    }
    if (options.store == "steam") {
        return publish_steam(options, loaded);
    }
    return publish_itch(options, loaded);
}

} // namespace slash::exporter
