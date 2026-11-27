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
#include <initializer_list>
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
    std::string channel;       // itch.io's
    std::string depot;         // Steam's
    std::string artifact;      // the Epic Games Store's
    std::string build_version; // the Epic Games Store's: the version and the platform
    fs::path output;           // relative to the project, as presets name it
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

// The builds a publish to the Epic Games Store uploads, in order, each with its artifact and the
// build version Epic gets: the game's version and the platform, since an artifact takes each
// version once whatever its platform.
Result<std::vector<Build>> epic_builds(const EpicStore& epic, const Presets& presets,
                                       const std::vector<std::string>& targets,
                                       const std::string& version) {
    std::vector<std::string> names = targets;
    if (names.empty()) {
        for (const auto& [name, artifact] : epic.artifacts) {
            names.push_back(name);
        }
    }
    if (names.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "nothing to publish: list the game's artifacts in " +
                              std::string(kPresetsFile) + "'s stores.epic.artifacts");
    }
    std::vector<Build> builds;
    std::set<std::string, std::less<>> uploads;
    for (const std::string& name : names) {
        auto preset = find_preset(presets, name);
        if (!preset) {
            return preset.error();
        }
        Build build;
        build.preset = std::move(preset).value();
        if (epic_platform(build.preset.platform).empty()) {
            return make_error(ErrorCode::InvalidArgument,
                              "\"" + name + "\" builds for " + build.preset.platform +
                                  ", which the Epic Games Store doesn't take");
        }
        for (const std::string& key : {name, build.preset.name, build.preset.platform}) {
            if (const auto it = epic.artifacts.find(key); it != epic.artifacts.end()) {
                build.artifact = it->second;
                break;
            }
        }
        if (build.artifact.empty()) {
            return make_error(ErrorCode::InvalidArgument,
                              "\"" + name +
                                  "\" has no Epic Games Store artifact: add its artifact id to "
                                  "stores.epic.artifacts, by preset or by platform");
        }
        build.build_version = version + "-" + std::string(epic_platform(build.preset.platform));
        if (!uploads.insert(build.artifact + " " + build.build_version).second) {
            return make_error(ErrorCode::InvalidArgument,
                              "two builds go to Epic artifact " + build.artifact + " as " +
                                  build.build_version +
                                  ", which takes each version once; publish \"" + name +
                                  "\" on its own, or give it an artifact of its own");
        }
        build.output = output_of(build.preset);
        builds.push_back(std::move(build));
    }
    return builds;
}

// BuildPatchTool's program: SLASH_BUILDPATCHTOOL_PATH's, else BuildPatchTool on PATH.
Result<std::string> find_build_patch_tool() {
    if (std::string configured = detail::environment(kBuildPatchToolPath); !configured.empty()) {
        if (!platform::exists(configured)) {
            return make_error(ErrorCode::NotFound, std::string(kBuildPatchToolPath) + " names \"" +
                                                       configured + "\", which doesn't exist");
        }
        return configured;
    }
    if (std::string found = detail::find_on_path("BuildPatchTool"); !found.empty()) {
        return found;
    }
    return make_error(ErrorCode::NotFound,
                      "uploading to the Epic Games Store needs BuildPatchTool, Epic's upload "
                      "tool, which isn't on PATH: download it from the Epic Developer Portal's "
                      "Downloads page, or set " +
                          std::string(kBuildPatchToolPath) + " to its program");
}

// The program a build's players launch, relative to its output: what Epic's launcher runs.
std::string epic_app_launch(const Build& build, const std::string& binary) {
    if (build.preset.platform == "macos") {
        return binary + ".app/Contents/MacOS/" + binary;
    }
    return binary + ".exe";
}

// What every BuildPatchTool command starts with: the program, the game's ids and the client's
// credentials, the secret named by its variable so it never shows in a command line.
std::vector<std::string> epic_command(const std::string& tool, const EpicStore& epic,
                                      const Build& build, const std::string& client) {
    return {tool,
            "-OrganizationId=" + epic.organization,
            "-ProductId=" + epic.product,
            "-ArtifactId=" + build.artifact,
            "-ClientId=" + client,
            "-ClientSecretEnvVar=" + std::string(kEpicClientSecret)};
}

// The command that uploads a build's output, read from root, with cloud as BuildPatchTool's
// working folder.
std::vector<std::string> epic_upload(const std::string& tool, const EpicStore& epic,
                                     const Build& build, const std::string& client,
                                     const std::string& binary, const fs::path& root,
                                     const fs::path& cloud) {
    std::vector<std::string> command = epic_command(tool, epic, build, client);
    command.insert(command.end(),
                   {"-mode=UploadBinary", "-BuildRoot=" + display(root),
                    "-CloudDir=" + display(cloud), "-BuildVersion=" + build.build_version,
                    "-AppLaunch=" + epic_app_launch(build, binary), "-AppArgs="});
    return command;
}

// The command that labels an uploaded build, which sets it live in the label's sandbox.
std::vector<std::string> epic_label(const std::string& tool, const EpicStore& epic,
                                    const Build& build, const std::string& client) {
    std::vector<std::string> command = epic_command(tool, epic, build, client);
    command.insert(command.end(), {"-mode=LabelBinary", "-BuildVersion=" + build.build_version,
                                   "-Label=" + epic.label,
                                   "-Platform=" + std::string(epic_platform(build.preset.platform)),
                                   "-SandboxId=" + epic.sandbox});
    return command;
}

PublishResult publish_epic(const PublishOptions& options, const Loaded& loaded) {
    if (!loaded.stores.epic) {
        return fail("options",
                    std::string(kPresetsFile) +
                        " has no stores.epic: add {\"organization\": \"<id>\", \"product\": "
                        "\"<id>\", \"artifacts\": {\"<preset or platform>\": \"<artifact id>\"}}, "
                        "from the game's Epic Developer Portal pages");
    }
    const EpicStore& epic = *loaded.stores.epic;
    auto builds = epic_builds(epic, loaded.presets, options.targets, loaded.version);
    if (!builds) {
        return fail("options", builds.error().message);
    }
    auto project = runtime::load_project(options.project);
    if (!project) {
        return fail("options", project.error().message);
    }
    const std::string binary = project.value().binary_name();

    std::string tool = "BuildPatchTool";
    std::string client = "$" + std::string(kEpicClientId);
    if (options.upload) {
        auto found = find_build_patch_tool();
        if (!found) {
            return fail("setup", found.error().message);
        }
        tool = std::move(found).value();
        client = detail::environment(kEpicClientId);
        if (client.empty() || detail::environment(kEpicClientSecret).empty()) {
            return fail("setup", "uploading to the Epic Games Store needs a BuildPatchTool "
                                 "client's id in " +
                                     std::string(kEpicClientId) + " and its secret in " +
                                     std::string(kEpicClientSecret) +
                                     ": make the client on the product's BuildPatchTool "
                                     "Credentials page in the Epic Developer Portal");
        }
    }

    const fs::path cloud = fs::path(".slash") / "publish" / "epic" / "cloud";
    PublishResult result;
    json::Value& out = result.json;
    out["store"] = options.store;
    out["organization"] = epic.organization;
    out["product"] = epic.product;
    if (!epic.label.empty()) {
        out["label"] = epic.label;
        out["sandbox"] = epic.sandbox;
    }
    out["version"] = loaded.version;
    out["upload"] = options.upload;
    json::Array planned;
    for (const Build& build : builds.value()) {
        json::Value item;
        item["preset"] = build.preset.name;
        item["platform"] = build.preset.platform;
        item["artifact"] = build.artifact;
        item["build_version"] = build.build_version;
        item["output"] = display(build.output);
        item["app_launch"] = epic_app_launch(build, binary);
        json::Array commands;
        commands.push_back(command_json(
            epic_upload("BuildPatchTool", epic, build, client, binary, build.output, cloud)));
        if (!epic.label.empty()) {
            commands.push_back(command_json(epic_label("BuildPatchTool", epic, build, client)));
        }
        item["commands"] = std::move(commands);
        planned.push_back(std::move(item));
    }
    if (!options.upload) {
        out["builds"] = std::move(planned);
        out["build"] = options.build;
        out["needs"] = json::Array{std::string("BuildPatchTool on PATH or in ") +
                                       std::string(kBuildPatchToolPath),
                                   std::string(kEpicClientId), std::string(kEpicClientSecret)};
        result.ok = true;
        return result;
    }

    const fs::path log_dir = publish_dir(options);
    std::error_code error;
    const fs::path cloud_dir = fs::absolute(fs::path(options.project) / cloud, error);
    fs::create_directories(cloud_dir, error);
    json::Array pushed;
    for (std::size_t i = 0; i < builds.value().size(); ++i) {
        const Build& build = builds.value()[i];
        json::Value& item = planned[i];
        if (std::optional<PublishResult> failed = export_build(build, options, item, pushed)) {
            return std::move(*failed);
        }
        const std::string target = "artifact " + build.artifact + " as " + build.build_version;
        platform::ProcessOptions process;
        process.log_path = (log_dir / (options.store + "-" + build.preset.name + ".log")).string();
        process.tail_lines = 20;
        fs::remove(process.log_path, error);
        std::vector<std::vector<std::string>> steps{
            epic_upload(tool, epic, build, client, binary,
                        fs::absolute(fs::path(options.project) / build.output, error), cloud_dir)};
        if (!epic.label.empty()) {
            steps.push_back(epic_label(tool, epic, build, client));
        }
        platform::ProcessResult last;
        for (const std::vector<std::string>& step : steps) {
            auto ran = platform::run_process(step, process);
            if (!ran || ran.value().exit_code != 0) {
                const bool labelling = &step != &steps.front();
                json::Value details;
                details["preset"] = build.preset.name;
                details["artifact"] = build.artifact;
                details["log"] = display(fs::path(process.log_path));
                std::string message;
                if (!ran) {
                    message = "BuildPatchTool couldn't be started: " + ran.error().message;
                } else {
                    details["exit_code"] = ran.value().exit_code;
                    details["tail"] = ran.value().tail;
                    message = labelling ? "BuildPatchTool uploaded \"" + build.preset.name +
                                              "\" but couldn't label it " + epic.label +
                                              ": label " + build.build_version +
                                              " in the Epic Developer Portal, or publish again"
                                        : "BuildPatchTool couldn't upload \"" + build.preset.name +
                                              "\" to " + target;
                }
                if (labelling) {
                    pushed.emplace_back(target);
                }
                return fail("push", std::move(message), std::move(details), std::move(pushed));
            }
            last = std::move(ran).value();
        }
        json::Value push;
        push["log"] = display(fs::path(process.log_path));
        push["tail"] = last.tail;
        item["push"] = std::move(push);
        pushed.emplace_back(target);
    }
    out["builds"] = std::move(planned);
    result.ok = true;
    return result;
}

// text with each secret in it replaced, so a tool's output can't carry a password on.
std::string redacted(std::string text, std::initializer_list<std::string_view> secrets) {
    for (const std::string_view secret : secrets) {
        if (secret.empty()) {
            continue;
        }
        for (std::size_t at = text.find(secret); at != std::string::npos;
             at = text.find(secret, at + 3)) {
            text.replace(at, secret.size(), "***");
        }
    }
    return text;
}

// The builds a publish to GOG uploads, in order: the presets named, else stores.gog.presets,
// else every preset GOG takes; one per platform, since a platform has one build at a time.
Result<std::vector<Build>> gog_builds(const GogStore& gog, const Presets& presets,
                                      const std::vector<std::string>& targets) {
    std::vector<std::string> names = targets.empty() ? gog.presets : targets;
    if (names.empty()) {
        for (const auto& [name, preset] : presets) {
            if (!gog_platform(preset.platform).empty()) {
                names.push_back(name);
            }
        }
    }
    if (names.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "nothing to publish: GOG takes Windows and macOS builds, and " +
                              std::string(kPresetsFile) + " has neither");
    }
    std::vector<Build> builds;
    std::set<std::string, std::less<>> platforms;
    for (const std::string& name : names) {
        auto preset = find_preset(presets, name);
        if (!preset) {
            return preset.error();
        }
        Build build;
        build.preset = std::move(preset).value();
        if (gog_platform(build.preset.platform).empty()) {
            return make_error(ErrorCode::InvalidArgument, "\"" + name + "\" builds for " +
                                                              build.preset.platform +
                                                              ", which GOG doesn't take");
        }
        if (!platforms.insert(build.preset.platform).second) {
            return make_error(ErrorCode::InvalidArgument,
                              "two builds are for " + build.preset.platform +
                                  ", and GOG keeps one build per platform; publish \"" + name +
                                  "\" on its own");
        }
        build.output = output_of(build.preset);
        builds.push_back(std::move(build));
    }
    return builds;
}

// GOG's Pipeline Builder: SLASH_GOG_PIPELINE_BUILDER_PATH's, else GOGGalaxyPipelineBuilder on
// PATH.
Result<std::string> find_pipeline_builder() {
    if (std::string configured = detail::environment(kGogPipelineBuilderPath);
        !configured.empty()) {
        if (!platform::exists(configured)) {
            return make_error(ErrorCode::NotFound, std::string(kGogPipelineBuilderPath) +
                                                       " names \"" + configured +
                                                       "\", which doesn't exist");
        }
        return configured;
    }
    if (std::string found = detail::find_on_path("GOGGalaxyPipelineBuilder"); !found.empty()) {
        return found;
    }
    return make_error(ErrorCode::NotFound,
                      "uploading to GOG needs GOG's Pipeline Builder, which isn't on PATH: "
                      "download it from the GOG Developer Portal, or set " +
                          std::string(kGogPipelineBuilderPath) + " to its program");
}

// Where publish gog writes a build's Pipeline Builder project, relative to the project.
fs::path gog_project_path(const Build& build) {
    return fs::path(".slash") / "publish" / "gog" / (build.preset.name + ".json");
}

// The Pipeline Builder project that builds and uploads a build: the game's product, with one
// depot holding the build's output (folder) and the task that starts the game.
json::Value gog_project(const GogStore& gog, const Build& build, const std::string& name,
                        const std::string& binary, const std::string& version,
                        const std::string& folder) {
    json::Array languages;
    for (const std::string& language : gog.languages) {
        languages.emplace_back(language);
    }
    json::Value depot;
    depot["name"] = name + " (" + std::string(gog_platform(build.preset.platform)) + ")";
    depot["folder"] = folder;
    depot["languages"] = languages;
    json::Value task;
    task["type"] = "FileTask";
    task["category"] = "game";
    task["name"] = name;
    task["path"] = build.preset.platform == "macos" ? binary + ".app" : binary + ".exe";
    task["isPrimary"] = true;
    task["isHidden"] = false;
    task["languages"] = json::Array{"*"};
    json::Value product;
    product["name"] = name;
    product["productId"] = gog.product;
    product["depots"] = json::Array{std::move(depot)};
    product["tasks"] = json::Array{std::move(task)};
    json::Value project;
    project["baseProductId"] = gog.product;
    project["clientId"] = "";
    project["clientSecret"] = "";
    project["installDirectory"] = name;
    project["name"] = name;
    project["platform"] = gog_platform(build.preset.platform);
    project["version"] = version;
    project["scriptInterpreter"] = true;
    project["dependencies"] = json::Array{};
    project["products"] = json::Array{std::move(product)};
    json::Value out;
    out["project"] = std::move(project);
    return out;
}

// The command that builds and uploads a Pipeline Builder project, signed in as user, setting it
// live on the store's branch when it has one.
std::vector<std::string> gog_command(const std::string& builder, const GogStore& gog,
                                     const std::string& project, const std::string& version,
                                     const std::string& user, const std::string& password,
                                     const std::string& branch_password) {
    std::vector<std::string> command{builder,      "build-game", project,     "--username", user,
                                     "--password", password,     "--version", version};
    if (!gog.branch.empty()) {
        command.insert(command.end(), {"--branch", gog.branch});
        if (!branch_password.empty()) {
            command.insert(command.end(), {"--branch_password", branch_password});
        }
    }
    return command;
}

PublishResult publish_gog(const PublishOptions& options, const Loaded& loaded) {
    if (!loaded.stores.gog) {
        return fail("options", std::string(kPresetsFile) +
                                   " has no stores.gog: add {\"product\": \"<product id>\"}, the "
                                   "game's id from the GOG Developer Portal");
    }
    const GogStore& gog = *loaded.stores.gog;
    auto builds = gog_builds(gog, loaded.presets, options.targets);
    if (!builds) {
        return fail("options", builds.error().message);
    }
    auto project = runtime::load_project(options.project);
    if (!project) {
        return fail("options", project.error().message);
    }
    const std::string name = project.value().name;
    const std::string binary = project.value().binary_name();

    std::string builder = "GOGGalaxyPipelineBuilder";
    std::string user = "$" + std::string(kGogUser);
    std::string password = "$" + std::string(kGogPassword);
    std::string branch_password = detail::environment(kGogBranchPassword).empty()
                                      ? ""
                                      : "$" + std::string(kGogBranchPassword);
    if (options.upload) {
        auto found = find_pipeline_builder();
        if (!found) {
            return fail("setup", found.error().message);
        }
        builder = std::move(found).value();
        user = detail::environment(kGogUser);
        password = detail::environment(kGogPassword);
        branch_password = detail::environment(kGogBranchPassword);
        if (user.empty() || password.empty()) {
            return fail("setup", "uploading to GOG needs the GOG account Pipeline Builder signs "
                                 "in as, in " +
                                     std::string(kGogUser) + " and " + std::string(kGogPassword) +
                                     ": an account with access to the game in the GOG "
                                     "Developer Portal");
        }
    }

    PublishResult result;
    json::Value& out = result.json;
    out["store"] = options.store;
    out["product"] = gog.product;
    if (!gog.branch.empty()) {
        out["branch"] = gog.branch;
    }
    out["version"] = loaded.version;
    out["upload"] = options.upload;
    json::Array planned;
    for (const Build& build : builds.value()) {
        json::Value item;
        item["preset"] = build.preset.name;
        item["platform"] = build.preset.platform;
        item["output"] = display(build.output);
        item["project"]["path"] = display(gog_project_path(build));
        item["project"]["json"] =
            gog_project(gog, build, name, binary, loaded.version, display(build.output));
        item["command"] = command_json(gog_command(
            "GOGGalaxyPipelineBuilder", gog, display(gog_project_path(build)), loaded.version,
            "$" + std::string(kGogUser), "$" + std::string(kGogPassword),
            branch_password.empty() ? "" : "$" + std::string(kGogBranchPassword)));
        planned.push_back(std::move(item));
    }
    if (!options.upload) {
        out["builds"] = std::move(planned);
        out["build"] = options.build;
        out["needs"] = json::Array{std::string("GOGGalaxyPipelineBuilder on PATH or in ") +
                                       std::string(kGogPipelineBuilderPath),
                                   std::string(kGogUser), std::string(kGogPassword)};
        result.ok = true;
        return result;
    }

    const fs::path log_dir = publish_dir(options);
    std::error_code error;
    json::Array pushed;
    for (std::size_t i = 0; i < builds.value().size(); ++i) {
        const Build& build = builds.value()[i];
        json::Value& item = planned[i];
        if (std::optional<PublishResult> failed = export_build(build, options, item, pushed)) {
            return std::move(*failed);
        }
        // The file an upload writes names the output by its absolute path.
        const fs::path file =
            fs::absolute(fs::path(options.project) / gog_project_path(build), error);
        fs::create_directories(file.parent_path(), error);
        const json::Value written = gog_project(
            gog, build, name, binary, loaded.version,
            fs::absolute(fs::path(options.project) / build.output, error).generic_string());
        if (auto saved = platform::write_file(file.string(), json::write(written) + "\n"); !saved) {
            return fail("push", saved.error().message, {}, std::move(pushed));
        }
        const std::string target = "product " + gog.product + " (" +
                                   std::string(gog_platform(build.preset.platform)) + ")";
        platform::ProcessOptions process;
        process.log_path = (log_dir / (options.store + "-" + build.preset.name + ".log")).string();
        process.tail_lines = 20;
        fs::remove(process.log_path, error);
        auto ran = platform::run_process(gog_command(builder, gog, file.string(), loaded.version,
                                                     user, password, branch_password),
                                         process);
        if (!ran || ran.value().exit_code != 0) {
            json::Value details;
            details["preset"] = build.preset.name;
            details["log"] = display(fs::path(process.log_path));
            std::string message;
            if (!ran) {
                message = "Pipeline Builder couldn't be started: " + ran.error().message;
            } else {
                details["exit_code"] = ran.value().exit_code;
                details["tail"] = redacted(ran.value().tail, {password, branch_password});
                message =
                    "Pipeline Builder couldn't upload \"" + build.preset.name + "\" to " + target;
            }
            return fail("push", std::move(message), std::move(details), std::move(pushed));
        }
        json::Value push;
        push["log"] = display(fs::path(process.log_path));
        push["tail"] = redacted(ran.value().tail, {password, branch_password});
        item["push"] = std::move(push);
        pushed.emplace_back(target);
    }
    out["builds"] = std::move(planned);
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

// export_presets.json's stores.epic, or null.
const json::Value* epic_section(const json::Value& presets) {
    const json::Value* stores = presets.find("stores");
    return stores == nullptr ? nullptr : stores->find("epic");
}

// stores.epic's checks: the ids' forms, artifacts naming Windows or macOS builds, and a label
// with the sandbox it sets the build live in.
void validate_epic(const json::Value& presets, std::vector<json::ValidationError>& errors) {
    const json::Value* epic = epic_section(presets);
    if (epic == nullptr) {
        return;
    }
    for (const char* key : {"organization", "product", "sandbox"}) {
        if (const json::Value* id = epic->find(key);
            id != nullptr && !name_of(id->as_string(), "-_")) {
            errors.push_back({std::string("/stores/epic/") + key,
                              "expected the id from the Epic Developer Portal: letters, digits, "
                              "'-' or '_'"});
        }
    }
    if (const json::Value* artifacts = epic->find("artifacts"); artifacts != nullptr) {
        for (const auto& [name, artifact] : artifacts->as_object()) {
            const std::string pointer = "/stores/epic/artifacts/" + json::pointer_escape(name);
            if (!name_of(artifact.as_string(), "-_")) {
                errors.push_back({pointer, "expected the artifact id from the Epic Developer "
                                           "Portal: letters, digits, '-' or '_'"});
            } else if (const auto platform = platform_of(presets, name); !platform) {
                errors.push_back({pointer, "no preset or platform called \"" + name + "\""});
            } else if (epic_platform(*platform).empty()) {
                errors.push_back({pointer, "the Epic Games Store takes Windows and macOS builds, "
                                           "not " +
                                               *platform + " ones"});
            }
        }
    }
    const json::Value* label = epic->find("label");
    if (label != nullptr && !name_of(label->as_string(), "._-")) {
        errors.push_back(
            {"/stores/epic/label", "labels are letters, digits, '.', '_' or '-', such as Live"});
    }
    if ((label == nullptr) != (epic->find("sandbox") == nullptr)) {
        errors.push_back({"/stores/epic",
                          "label and sandbox go together: a label sets builds live in a "
                          "sandbox, such as Live in the game's Live sandbox"});
    }
}

// export_presets.json's stores.gog, or null.
const json::Value* gog_section(const json::Value& presets) {
    const json::Value* stores = presets.find("stores");
    return stores == nullptr ? nullptr : stores->find("gog");
}

// stores.gog's checks: a digit product id, presets GOG takes, language codes and a private
// branch.
void validate_gog(const json::Value& presets, std::vector<json::ValidationError>& errors) {
    const json::Value* gog = gog_section(presets);
    if (gog == nullptr) {
        return;
    }
    if (const json::Value* product = gog->find("product");
        product != nullptr && !digits(product->as_string())) {
        errors.push_back({"/stores/gog/product",
                          "expected the product id's digits, from the GOG Developer "
                          "Portal"});
    }
    if (const json::Value* list = gog->find("presets"); list != nullptr) {
        const json::Array& names = list->as_array();
        for (std::size_t i = 0; i < names.size(); ++i) {
            const std::string pointer = "/stores/gog/presets/" + std::to_string(i);
            const std::string& name = names[i].as_string();
            if (const auto platform = platform_of(presets, name); !platform) {
                errors.push_back({pointer, "no preset or platform called \"" + name + "\""});
            } else if (gog_platform(*platform).empty()) {
                errors.push_back(
                    {pointer, "GOG takes Windows and macOS builds, not " + *platform + " ones"});
            }
        }
    }
    if (const json::Value* languages = gog->find("languages"); languages != nullptr) {
        const json::Array& codes = languages->as_array();
        for (std::size_t i = 0; i < codes.size(); ++i) {
            if (!name_of(codes[i].as_string(), "-")) {
                errors.push_back({"/stores/gog/languages/" + std::to_string(i),
                                  "expected a language code, such as en-US"});
            }
        }
    }
    if (const json::Value* branch = gog->find("branch"); branch != nullptr) {
        std::string lower = branch->as_string();
        std::ranges::transform(lower, lower.begin(), [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        });
        if (!name_of(branch->as_string(), "._-")) {
            errors.push_back(
                {"/stores/gog/branch", "branch names are letters, digits, '.', '_' or '-'"});
        } else if (lower == "master") {
            errors.push_back({"/stores/gog/branch",
                              "Pipeline Builder publishes to private branches only: leave "
                              "branch out and publish the build to master in the Developer "
                              "Portal"});
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

    json::Value epic_id = property("string", "An id from the Epic Developer Portal.");
    epic_id["maxLength"] = kMaxStoreName;
    json::Value artifacts = property(
        "object", "Artifact ids by preset, or by platform for its presets, such as {\"windows\": "
                  "\"<artifact id>\"}. One artifact can take a Windows and a macOS build. The "
                  "Epic Games Store takes Windows and macOS builds.");
    artifacts["additionalProperties"] = epic_id;
    artifacts["minProperties"] = 1;
    json::Value epic;
    epic["type"] = "object";
    epic["description"] = "Where publish epic sends builds, through BuildPatchTool, as the "
                          "client in " +
                          std::string(kEpicClientId) + " and " + std::string(kEpicClientSecret) +
                          ".";
    epic["additionalProperties"] = false;
    epic["required"] = json::Array{"organization", "product", "artifacts"};
    epic["properties"]["organization"] =
        property("string", "The organization id, from the Epic Developer Portal.");
    epic["properties"]["organization"]["maxLength"] = kMaxStoreName;
    epic["properties"]["product"] = property("string", "The product id of the game.");
    epic["properties"]["product"]["maxLength"] = kMaxStoreName;
    epic["properties"]["artifacts"] = std::move(artifacts);
    epic["properties"]["label"] =
        property("string", "The label set on each uploaded build, such as \"Live\", which sets "
                           "it live in the sandbox. Default: none, labelled in the Portal.");
    epic["properties"]["label"]["maxLength"] = kMaxStoreName;
    epic["properties"]["sandbox"] =
        property("string", "The sandbox id the label is set in; goes with label.");
    epic["properties"]["sandbox"]["maxLength"] = kMaxStoreName;

    json::Value gog_product =
        property("string", "The game's product id, from the GOG Developer Portal.");
    gog_product["maxLength"] = kMaxStoreName;
    json::Value gog_presets =
        property("array", "Presets, or platforms for their presets, to publish, one per "
                          "platform. GOG takes Windows and macOS builds. Default: every preset "
                          "GOG takes.");
    gog_presets["items"]["type"] = "string";
    json::Value gog_languages =
        property("array", "The languages the game's depot holds, as codes such as \"en-US\". "
                          "Default: [\"en-US\"].");
    gog_languages["items"]["type"] = "string";
    gog_languages["minItems"] = 1;
    json::Value gog_branch =
        property("string", "The private branch each build is published to, such as "
                           "\"staging\". Default: none, published in the Developer Portal.");
    gog_branch["maxLength"] = kMaxStoreName;
    json::Value gog;
    gog["type"] = "object";
    gog["description"] = "Where publish gog sends builds, through GOG's Pipeline Builder, signed "
                         "in as " +
                         std::string(kGogUser) + ".";
    gog["additionalProperties"] = false;
    gog["required"] = json::Array{"product"};
    gog["properties"]["product"] = std::move(gog_product);
    gog["properties"]["presets"] = std::move(gog_presets);
    gog["properties"]["languages"] = std::move(gog_languages);
    gog["properties"]["branch"] = std::move(gog_branch);

    json::Value stores;
    stores["type"] = "object";
    stores["description"] = "Where slash-cli publish sends the presets' builds, by store.";
    stores["additionalProperties"] = false;
    stores["properties"]["itch"] = std::move(itch);
    stores["properties"]["steam"] = std::move(steam);
    stores["properties"]["epic"] = std::move(epic);
    stores["properties"]["gog"] = std::move(gog);
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

std::string_view gog_platform(std::string_view platform) {
    if (platform == "windows") {
        return "windows";
    }
    if (platform == "macos") {
        return "osx";
    }
    return {};
}

std::string_view epic_platform(std::string_view platform) {
    if (platform == "windows") {
        return "Windows";
    }
    if (platform == "macos") {
        return "Mac";
    }
    return {};
}

std::vector<json::ValidationError> validate_stores(const json::Value& presets) {
    std::vector<json::ValidationError> errors;
    validate_steam(presets, errors);
    validate_epic(presets, errors);
    validate_gog(presets, errors);
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
    if (const json::Value* epic = epic_section(presets); epic != nullptr) {
        EpicStore store;
        store.organization = epic->find("organization")->as_string();
        store.product = epic->find("product")->as_string();
        for (const auto& [name, artifact] : epic->find("artifacts")->as_object()) {
            store.artifacts.emplace(name, artifact.as_string());
        }
        if (const json::Value* label = epic->find("label"); label != nullptr) {
            store.label = label->as_string();
        }
        if (const json::Value* sandbox = epic->find("sandbox"); sandbox != nullptr) {
            store.sandbox = sandbox->as_string();
        }
        stores.epic = std::move(store);
    }
    if (const json::Value* gog = gog_section(presets); gog != nullptr) {
        GogStore store;
        store.product = gog->find("product")->as_string();
        if (const json::Value* list = gog->find("presets"); list != nullptr) {
            for (const json::Value& name : list->as_array()) {
                store.presets.push_back(name.as_string());
            }
        }
        if (const json::Value* languages = gog->find("languages"); languages != nullptr) {
            store.languages.clear();
            for (const json::Value& language : languages->as_array()) {
                store.languages.push_back(language.as_string());
            }
        }
        if (const json::Value* branch = gog->find("branch"); branch != nullptr) {
            store.branch = branch->as_string();
        }
        stores.gog = std::move(store);
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
    if (options.store == "epic") {
        return publish_epic(options, loaded);
    }
    if (options.store == "gog") {
        return publish_gog(options, loaded);
    }
    return publish_itch(options, loaded);
}

} // namespace slash::exporter
